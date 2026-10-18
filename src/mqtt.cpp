/**
 * @file mqtt.cpp "mqtt.cpp"
 * @brief iown-homecontrol 1W controller exposed to Home Assistant over MQTT/WiFi
 *
 * Emulates the paired 1W controller for each configured blind (one HA cover per
 * channel address) and sends authenticated OPEN / CLOSE / STOP(=My) commands.
 * Covers are optimistic (1W has no position feedback). Each channel's rolling
 * counter is kept in NVS and can be resynced over MQTT after a fresh sniff.
 *
 * Secrets: include/iown_key.h (IOWN_PRODUCT_KEY) and include/iown_net.h
 * (WiFi/MQTT), both git-ignored. Per-channel keys are derived at boot from the
 * productKey and the channel address.
 *
 * Build/flash: pio run -e ttgo-lora32-v21-mqtt -t upload
 *
 * MIT License
 * Copyright (c) Velocet
 */

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <WiFi.h>
#include <PubSubClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <mbedtls/aes.h>

#include <LoRa32.h>
#include <RadioLib.h>

#include "IoHome.h"
#include "iown_key.h" // IOWN_PRODUCT_KEY  (git-ignored, generated)
#include "iown_net.h" // WiFi + MQTT creds (git-ignored)

// io-homecontrol public transfer key (same value as scripts/Iown-ioCrypto.py)
static const uint8_t TRANSFER_KEY[16] = {
  0x34,0xc3,0x46,0x6e,0xd8,0x8f,0x4e,0x8e,0x16,0xaa,0x47,0x39,0x49,0x88,0x43,0x73};
static const uint8_t PRODUCT_KEY[16] = IOWN_PRODUCT_KEY; // from git-ignored iown_key.h

// --- blind configuration (addresses are not secret) ---
struct Blind {
  const char* name;      // HA entity name
  uint8_t     addr[3];   // channel source address, frame order
  uint16_t    seed;      // initial counter if NVS is empty (set a bit above last sniffed)
  uint8_t     key[16];   // derived at boot
};
static Blind BLINDS[] = {
  {"Blind_1", {0xB5,0x1A,0x3F}, 0x3700, {0}}, // channel 2 address
  {"Blind_2", {0xB5,0x1A,0x40}, 0x0600, {0}}, // channel 1 address
};
static const size_t NBLINDS = sizeof(BLINDS) / sizeof(BLINDS[0]);

// command payloads (data = command ID + parameters, as captured from the Situo)
static const uint8_t CMD_OPEN[]  = {0x00, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00};
static const uint8_t CMD_CLOSE[] = {0x00, 0x01, 0x43, 0xC8, 0x00, 0x00, 0x00};
static const uint8_t CMD_STOP[]  = {0x00, 0x01, 0x43, 0xD2, 0x00, 0x20, 0xD2, 0x00, 0x00};

LORA32_RADIO radio = new Module(LORA32_SPI_CS, LORA32_RADIO_IO0, LORA32_RADIO_RST, LORA32_RADIO_IO1);
WiFiClient    wifiClient;
PubSubClient  mqtt(wifiClient);
Preferences   prefs;

// =================== crypto / framing (mirrors scripts/Iown-ioCrypto.py) ===================
static void aesEncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

// key = AES(transfer_key, IV_from_address) XOR productKey  (port of encrypt_1W_key)
static void deriveKey(const uint8_t addr[3], uint8_t out[16]) {
  uint8_t iv[16], enc[16];
  for(int i = 0; i < 15; i += 3) {iv[i] = addr[0];iv[i+1] = addr[1];iv[i+2] = addr[2];}
  iv[15] = addr[0];
  aesEncryptBlock(TRANSFER_KEY, iv, enc);
  for(int i = 0; i < 16; i++) {out[i] = enc[i] ^ PRODUCT_KEY[i];}
}

static void ivChecksum(uint8_t frameByte, uint8_t* c1, uint8_t* c2) {
  uint8_t tmp = frameByte ^ *c2;
  uint8_t nc2 = (uint8_t)((*c1 & 0x7f) << 1);
  if((*c1 & 0x80) == 0) {
    if(tmp >= 128) {nc2 |= 1;}
    *c1 = nc2;
    *c2 = (uint8_t)(tmp << 1);
  } else {
    if(tmp >= 128) {nc2 |= 1;}
    *c1 = nc2 ^ 0x55;
    *c2 = (uint8_t)((tmp << 1) ^ 0x5b);
  }
}

static void buildIV(const uint8_t* data, size_t dataLen, const uint8_t counter[2], uint8_t iv[16]) {
  memset(iv, 0, 16);
  uint8_t c1 = 0, c2 = 0;
  for(size_t i = 0; i < dataLen; i++) {
    ivChecksum(data[i], &c1, &c2);
    if(i < 8) {iv[i] = data[i];}
  }
  for(size_t j = dataLen; j < 8; j++) {iv[j] = 0x55;}
  iv[8] = c1;iv[9] = c2;
  iv[10] = counter[0];iv[11] = counter[1];
  for(int i = 12; i < 16; i++) {iv[i] = 0x55;}
}

static void create1wMac(const uint8_t counter[2], const uint8_t key[16],
                        const uint8_t* data, size_t dataLen, uint8_t mac[6]) {
  uint8_t iv[16], enc[16];
  buildIV(data, dataLen, counter, iv);
  aesEncryptBlock(key, iv, enc);
  memcpy(mac, enc, 6);
}

static uint16_t crc8408(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  for(size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for(int b = 0; b < 8; b++) {crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);}
  }
  return crc;
}

static size_t buildFrame(const Blind& bl, const uint8_t* data, size_t dataLen,
                         uint8_t ctrl1, const uint8_t counter[2], uint8_t* out) {
  static const uint8_t dest[3] = {0x00, 0x00, 0x3F}; // broadcast
  size_t n = 0;
  size_t lenField = 1 + 3 + 3 + dataLen + 2 + 6; // ctrl1..mac
  out[n++] = (uint8_t)(0xE0 | lenField);
  out[n++] = ctrl1;
  memcpy(out + n, dest, 3);n += 3;
  memcpy(out + n, bl.addr, 3);n += 3;
  memcpy(out + n, data, dataLen);n += dataLen;
  out[n++] = counter[0];out[n++] = counter[1];
  create1wMac(counter, bl.key, data, dataLen, out + n);n += 6;
  uint16_t crc = crc8408(out, n);
  out[n++] = (uint8_t)(crc & 0xff);out[n++] = (uint8_t)(crc >> 8);
  return n;
}

static size_t uartEncode(const uint8_t* frame, size_t len, uint8_t* out, size_t outMax) {
  size_t bit = 0;
  auto put = [&](uint8_t v) {
    if(bit / 8 < outMax) {
      if(v) {out[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));}
      else  {out[bit / 8] &= (uint8_t)~(0x80 >> (bit % 8));}
    }
    bit++;
  };
  memset(out, 0xFF, outMax);
  for(size_t i = 0; i < len; i++) {
    put(0);
    for(int b = 0; b < 8; b++) {put((frame[i] >> b) & 1);}
    put(1);
  }
  return (bit + 7) / 8;
}

// =================== RX decode (for passive counter tracking) ===================
#define RX_RAW_LEN (48)
volatile bool rxFlag = false;
#if defined(ESP32)
  ICACHE_RAM_ATTR
#endif
static void onReceive() {rxFlag = true;}

static inline uint8_t rawBit(const uint8_t* raw, size_t n) {return (raw[n / 8] >> (7 - (n % 8))) & 1;}

// decode UART framed bytes (start 0, 8 data LSB-first, stop 1); returns decoded length
static size_t uartDecode(const uint8_t* raw, size_t rawLen, uint8_t* out, size_t outMax) {
  size_t bits = rawLen * 8, n = 0;
  for(size_t pos = 0; pos + 10 <= bits && n < outMax; pos += 10) {
    if(rawBit(raw, pos) != 0 || rawBit(raw, pos + 9) != 1) {break;} // framing error = end of frame
    uint8_t b = 0;
    for(uint8_t i = 0; i < 8; i++) {b |= rawBit(raw, pos + 1 + i) << i;}
    out[n++] = b;
  }
  return n;
}

static void enterReceive() {
  radio.fixedPacketLengthMode(RX_RAW_LEN);
  radio.setPacketReceivedAction(onReceive);
  radio.startReceive();
}

// =================== counters (NVS) ===================
// Keyed by channel address (not entity name) so a counter follows its blind
// even if the name<->address mapping changes. NVS keys must be <= 15 chars.
static String nvsKey(const Blind& bl) {
  char k[12];
  snprintf(k, sizeof(k), "c%02x%02x%02x", bl.addr[0], bl.addr[1], bl.addr[2]);
  return String(k);
}
static uint16_t counterGet(const Blind& bl) {
  return (uint16_t)prefs.getUShort(nvsKey(bl).c_str(), bl.seed);
}
static void counterSet(const Blind& bl, uint16_t v) {prefs.putUShort(nvsKey(bl).c_str(), v);}

// =================== radio TX ===================
static void sendCommand(Blind& bl, const uint8_t* data, size_t dataLen) {
  uint16_t c = counterGet(bl) + 1; // one past our last
  uint8_t counter[2] = {(uint8_t)(c >> 8), (uint8_t)(c & 0xff)};
  uint8_t frame[40], air[52];

  size_t flen = buildFrame(bl, data, dataLen, 0x00, counter, frame);
  size_t alen = uartEncode(frame, flen, air, sizeof(air));

  radio.clearPacketReceivedAction(); // leave RX cleanly before transmitting
  radio.standby();
  radio.fixedPacketLengthMode(alen);

  int16_t st = 0;
  for(int rep = 0; rep < 4; rep++) {
    buildFrame(bl, data, dataLen, rep == 0 ? 0x20 : 0x00, counter, frame);
    uartEncode(frame, flen, air, sizeof(air));
    st = radio.transmit(air, alen);
    delay(40);
  }
  counterSet(bl, c);
  Serial.printf("[TX] %s counter=0x%04X len=%u state=%d\n", bl.name, c, (unsigned)dataLen, st);
  rxFlag = false;  // ignore any self-triggered Rx event from the transmit
  enterReceive();  // back to listening (for passive counter tracking)
}

// =================== MQTT topics + discovery ===================
static String baseTopic(const Blind& bl) {return String("iown/") + bl.name;}

static void publishState(const Blind& bl, const char* state) {
  mqtt.publish((baseTopic(bl) + "/state").c_str(), state, true);
}
static void publishCounter(const Blind& bl) {
  mqtt.publish((baseTopic(bl) + "/counter").c_str(), String(counterGet(bl)).c_str(), true);
}

static void publishDiscovery(const Blind& bl) {
  String uid = String("iown_") + bl.name;
  JsonDocument dev;
  dev["identifiers"][0] = "iown_lora32";
  dev["name"] = "iown-homecontrol LoRa32";
  dev["manufacturer"] = "iown-homecontrol";

  // cover
  {
    JsonDocument d;
    d["name"] = bl.name;
    d["unique_id"] = uid;
    d["command_topic"] = baseTopic(bl) + "/set";
    d["state_topic"] = baseTopic(bl) + "/state";
    d["payload_open"] = "OPEN";
    d["payload_close"] = "CLOSE";
    d["payload_stop"] = "STOP";
    d["state_open"] = "open";
    d["state_closed"] = "closed";
    d["optimistic"] = true;
    d["device_class"] = "blind";
    d["device"] = dev;
    String out;serializeJson(d, out);
    mqtt.publish((String("homeassistant/cover/") + uid + "/config").c_str(), out.c_str(), true);
  }
  // "My" button (same stop frame; recalls favourite when idle)
  {
    JsonDocument d;
    d["name"] = String(bl.name) + " My";
    d["unique_id"] = uid + "_my";
    d["command_topic"] = baseTopic(bl) + "/my";
    d["payload_press"] = "MY";
    d["device"] = dev;
    String out;serializeJson(d, out);
    mqtt.publish((String("homeassistant/button/") + uid + "_my/config").c_str(), out.c_str(), true);
  }
}

// process a received frame: if it's a valid 1W frame from one of our blinds,
// advance that channel's stored counter so we stay ahead of the physical remote
static void processRx() {
  rxFlag = false;
  uint8_t raw[RX_RAW_LEN], frame[RX_RAW_LEN * 8 / 10];
  if(radio.readData(raw, RX_RAW_LEN) != RADIOLIB_ERR_NONE) {radio.startReceive();return;}

  size_t n = uartDecode(raw, RX_RAW_LEN, frame, sizeof(frame));
  if(n >= 1) {size_t want = 1 + (frame[0] & 0x1F) + 2;if(want <= n) {n = want;} else {n = 0;}}

  if(n >= 18 && (frame[0] & 0x20) && crc8408(frame, n) == 0) { // 1W frame, CRC valid
    uint16_t heard = ((uint16_t)frame[n - 10] << 8) | frame[n - 9];
    for(size_t i = 0; i < NBLINDS; i++) {
      Blind& bl = BLINDS[i];
      if(memcmp(frame + 5, bl.addr, 3) == 0 && heard > counterGet(bl)) {
        counterSet(bl, heard);
        mqtt.publish((String("iown/") + bl.name + "/counter").c_str(), String(heard).c_str(), true);
        Serial.printf("[RX] %s heard counter=0x%04X -> tracked\n", bl.name, heard);
      }
    }
  }
  radio.startReceive();
}

static void onMessage(char* topic, byte* payload, unsigned int len) {
  String t(topic), p;
  for(unsigned int i = 0; i < len; i++) {p += (char)payload[i];}

  for(size_t i = 0; i < NBLINDS; i++) {
    Blind& bl = BLINDS[i];
    String base = baseTopic(bl);
    if(t == base + "/set") {
      if(p == "OPEN")       {sendCommand(bl, CMD_OPEN, sizeof(CMD_OPEN));publishState(bl, "open");}
      else if(p == "CLOSE") {sendCommand(bl, CMD_CLOSE, sizeof(CMD_CLOSE));publishState(bl, "closed");}
      else if(p == "STOP")  {sendCommand(bl, CMD_STOP, sizeof(CMD_STOP));}
      publishCounter(bl);
    } else if(t == base + "/my") {
      sendCommand(bl, CMD_STOP, sizeof(CMD_STOP));
      publishCounter(bl);
    } else if(t == base + "/counter/set") {
      counterSet(bl, (uint16_t)p.toInt());
      publishCounter(bl);
      Serial.printf("[CTR] %s resynced to %u\n", bl.name, (unsigned)p.toInt());
    }
  }
}

static void mqttConnect() {
  while(!mqtt.connected()) {
    Serial.print(F("[MQTT] connecting... "));
    if(mqtt.connect(IOWN_MQTT_CLIENT, IOWN_MQTT_USER, IOWN_MQTT_PASS)) {
      Serial.println(F("ok"));
      for(size_t i = 0; i < NBLINDS; i++) {
        Blind& bl = BLINDS[i];
        mqtt.subscribe((baseTopic(bl) + "/set").c_str());
        mqtt.subscribe((baseTopic(bl) + "/my").c_str());
        mqtt.subscribe((baseTopic(bl) + "/counter/set").c_str());
        publishDiscovery(bl);
        publishCounter(bl);
      }
    } else {
      Serial.print(F("rc="));Serial.print(mqtt.state());Serial.println(F(" retry in 3s"));
      delay(3000);
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(300);

  prefs.begin("iown", false);
  for(size_t i = 0; i < NBLINDS; i++) {deriveKey(BLINDS[i].addr, BLINDS[i].key);}

  // self-test: derived Blind_2 key must reproduce a captured frame's MAC
  {
    const uint8_t data[7]    = {0x00,0x01,0x43,0x00,0x00,0x00,0x00};
    const uint8_t counter[2] = {0x36,0xA9};
    const uint8_t want[6]    = {0x4A,0xAE,0xAC,0x75,0x6E,0xE0};
    const uint8_t addr[3]    = {0xB5,0x1A,0x3F};
    uint8_t k[16], mac[6];
    deriveKey(addr, k);
    create1wMac(counter, k, data, sizeof(data), mac);
    if(memcmp(mac, want, 6) != 0) {Serial.println(F("[SELFTEST] key/MAC mismatch - halting"));while(true) {delay(1000);}}
    Serial.println(F("[SELFTEST] key derivation + MAC OK"));
  }

  int16_t st = radio.beginFSK(868.95, 38.4, 19.2, 125.0, 14, 60, false);
  if(st) {Serial.print(F("[RADIO] beginFSK: "));Serial.println(st);while(true) {delay(1000);}}
  uint8_t sync[IOHOME_SYNC_WORD_LEN] = {
    (uint8_t)(IOHOME_SYNC_WORD >> 16), (uint8_t)(IOHOME_SYNC_WORD >> 8), (uint8_t)IOHOME_SYNC_WORD};
  radio.setSyncWord(sync, IOHOME_SYNC_WORD_LEN);
  radio.setCRC(false);
  radio.setEncoding(RADIOLIB_ENCODING_NRZ);
  radio.setDataShaping(RADIOLIB_SHAPING_NONE);

  WiFi.mode(WIFI_STA);
  WiFi.begin(IOWN_WIFI_SSID, IOWN_WIFI_PASS);
  Serial.print(F("[WIFI] connecting"));
  while(WiFi.status() != WL_CONNECTED) {delay(400);Serial.print('.');}
  Serial.print(F(" "));Serial.println(WiFi.localIP());

  mqtt.setBufferSize(640); // HA discovery JSON is larger than the 256 default
  mqtt.setServer(IOWN_MQTT_HOST, IOWN_MQTT_PORT);
  mqtt.setCallback(onMessage);

  enterReceive(); // listen for the physical remote to passively track counters
}

void loop() {
  if(WiFi.status() != WL_CONNECTED) {WiFi.reconnect();delay(500);return;}
  if(!mqtt.connected()) {mqttConnect();}
  mqtt.loop();
  if(rxFlag) {processRx();}
}
