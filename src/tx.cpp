/**
 * @file tx.cpp "tx.cpp"
 * @brief iown-homecontrol 1W transmit test: send a single "Up" (open) command
 *
 * Emulates the paired 1W controller (IOWN_NODE_ID / IOWN_KEY from include/iown_key.h,
 * git-ignored) and transmits one authenticated command on 868.95 MHz.
 *
 * The rolling counter is supplied at build time via -DIOWN_TX_COUNTER=0x....
 * Capture a fresh press with the sniffer, then build with (that counter + 1).
 *
 * Build/flash: PLATFORMIO_BUILD_FLAGS="-DIOWN_TX_COUNTER=0x36C1" pio run -e ttgo-lora32-v21-tx -t upload
 *
 * MIT License
 * Copyright (c) Velocet
 */

#include <Arduino.h>
#include <SPI.h>
#include <string.h>
#include <mbedtls/aes.h>

#include <LoRa32.h>
#include <RadioLib.h>

#include "IoHome.h"
#include "iown_key.h" // IOWN_NODE_ID, IOWN_KEY (git-ignored, generated)

#ifndef IOWN_TX_COUNTER
  #error "Define IOWN_TX_COUNTER=0x.... (fresh sniffed counter + 1) at build time"
#endif

// Position command as sent by the Situo: cmd 0x00 (execute_function), param 01 43 <level> 00 00 00
// level 0x00 = fully open (Up), 0xC8 = fully closed (Down)
#ifndef IOWN_TX_LEVEL
  #define IOWN_TX_LEVEL 0x00
#endif
static const uint8_t IOWN_CMD_UP[] = {0x00, 0x01, 0x43, IOWN_TX_LEVEL, 0x00, 0x00, 0x00};

LORA32_RADIO radio = new Module(LORA32_SPI_CS, LORA32_RADIO_IO0, LORA32_RADIO_RST, LORA32_RADIO_IO1);

void halt(const char* what, int state) {Serial.print(what);Serial.println(state);while(true) {delay(1000);}}

// --- CRC (polynomial 0x8408, init 0, no final xor); compute over the whole buffer ---
static uint16_t crc8408(const uint8_t* data, size_t len) {
  uint16_t crc = 0;
  for(size_t i = 0; i < len; i++) {
    crc ^= data[i];
    for(int b = 0; b < 8; b++) {crc = (crc & 1) ? (crc >> 1) ^ 0x8408 : (crc >> 1);}
  }
  return crc;
}

// --- IV checksum helper (port of computeChecksum in scripts/Iown-ioCrypto.py) ---
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

// --- build the 1W IV for sequence_number (counter) mode (port of constructInitialValue) ---
static void buildIV(const uint8_t* data, size_t dataLen, const uint8_t counter[2], uint8_t iv[16]) {
  memset(iv, 0, 16);
  uint8_t c1 = 0, c2 = 0;
  for(size_t i = 0; i < dataLen; i++) {
    ivChecksum(data[i], &c1, &c2);
    if(i < 8) {iv[i] = data[i];}
  }
  for(size_t j = dataLen; j < 8; j++) {iv[j] = 0x55;}
  iv[8] = c1;
  iv[9] = c2;
  iv[10] = counter[0];
  iv[11] = counter[1];
  for(int i = 12; i < 16; i++) {iv[i] = 0x55;}
}

static void aesEncryptBlock(const uint8_t key[16], const uint8_t in[16], uint8_t out[16]) {
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  mbedtls_aes_crypt_ecb(&ctx, MBEDTLS_AES_ENCRYPT, in, out);
  mbedtls_aes_free(&ctx);
}

// MAC = AES(key, IV)[:6]  (port of create_1W_hmac)
static void create1wMac(const uint8_t counter[2], const uint8_t key[16],
                        const uint8_t* data, size_t dataLen, uint8_t mac[6]) {
  uint8_t iv[16], enc[16];
  buildIV(data, dataLen, counter, iv);
  aesEncryptBlock(key, iv, enc);
  memcpy(mac, enc, 6);
}

// --- assemble a full 1W frame (without sync word): ctrl0 ctrl1 dest src data counter mac crc ---
// returns frame length
static size_t buildFrame(uint8_t ctrl1, const uint8_t counter[2], uint8_t* out) {
  static const uint8_t dest[3] = {0x00, 0x00, 0x3F};        // broadcast
  static const uint8_t src[3]  = IOWN_NODE_ID;
  static const uint8_t key[16] = IOWN_KEY;
  size_t n = 0;
  size_t lenField = 1 + 3 + 3 + sizeof(IOWN_CMD_UP) + 2 + 6; // ctrl1..mac
  out[n++] = (uint8_t)(0xE0 | lenField); // end + start + oneway + length
  out[n++] = ctrl1;
  memcpy(out + n, dest, 3);n += 3;
  memcpy(out + n, src, 3); n += 3;
  memcpy(out + n, IOWN_CMD_UP, sizeof(IOWN_CMD_UP));n += sizeof(IOWN_CMD_UP);
  out[n++] = counter[0];
  out[n++] = counter[1];
  create1wMac(counter, key, IOWN_CMD_UP, sizeof(IOWN_CMD_UP), out + n);n += 6;
  uint16_t crc = crc8408(out, n);
  out[n++] = (uint8_t)(crc & 0xff); // LSB first
  out[n++] = (uint8_t)(crc >> 8);
  return n;
}

// --- UART encode: each byte -> start(0) + 8 data bits LSB first + stop(1), packed MSB first ---
static size_t uartEncode(const uint8_t* frame, size_t len, uint8_t* out, size_t outMax) {
  size_t bit = 0;
  auto put = [&](uint8_t v) {
    if(bit / 8 < outMax) {
      if(v) {out[bit / 8] |= (uint8_t)(0x80 >> (bit % 8));}
      else  {out[bit / 8] &= (uint8_t)~(0x80 >> (bit % 8));}
    }
    bit++;
  };
  memset(out, 0xFF, outMax); // idle line is high; pads the final partial byte with 1s
  for(size_t i = 0; i < len; i++) {
    put(0);                                             // start bit
    for(int b = 0; b < 8; b++) {put((frame[i] >> b) & 1);} // data, LSB first
    put(1);                                             // stop bit
  }
  return (bit + 7) / 8;
}

void selfTest() {
  // MAC vector from docs/linklayer.md: key 0102..16, data 30 7e..01, seq 1234 -> 19e81ec43d5e
  const uint8_t k[16]   = {0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x10,0x11,0x12,0x13,0x14,0x15,0x16};
  const uint8_t d[17]   = {0x30,0x7e,0x60,0x49,0x1f,0x97,0x6a,0xdf,0x65,0x3d,0xb0,0xed,0x78,0x5e,0x49,0xa2,0x01};
  const uint8_t seq[2]  = {0x12,0x34};
  const uint8_t want[6] = {0x19,0xe8,0x1e,0xc4,0x3d,0x5e};
  uint8_t mac[6];
  create1wMac(seq, k, d, sizeof(d), mac);
  if(memcmp(mac, want, 6) != 0) {halt("[SELFTEST] MAC port mismatch", 0);}

  // CRC vector from a captured Up frame (public): whole frame incl. CRC must crc to 0
  const uint8_t f[] = {0xF6,0x00,0x00,0x00,0x3F,0xB5,0x1A,0x3F,0x00,0x01,0x43,0x00,0x00,0x00,0x00,
                       0x36,0xA9,0x4A,0xAE,0xAC,0x75,0x6E,0xE0,0x8A,0x69};
  if(crc8408(f, sizeof(f)) != 0) {halt("[SELFTEST] CRC port mismatch", 0);}
  Serial.println(F("[SELFTEST] MAC + CRC ports OK"));
}

void setup() {
  Serial.begin(115200);
  delay(500);
  selfTest();

  int16_t state = radio.beginFSK(868.95, 38.4, 19.2, 125.0, 14, 60, false); // 14 dBm, 60 byte preamble
  if(state) {halt("[RADIO] beginFSK: ", state);}

  uint8_t sync[IOHOME_SYNC_WORD_LEN] = {
    (uint8_t)(IOHOME_SYNC_WORD >> 16), (uint8_t)(IOHOME_SYNC_WORD >> 8), (uint8_t)IOHOME_SYNC_WORD};
  state = radio.setSyncWord(sync, IOHOME_SYNC_WORD_LEN);
  if(state) {halt("[RADIO] setSyncWord: ", state);}
  state = radio.setCRC(false);
  if(state) {halt("[RADIO] setCRC: ", state);}
  state = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
  if(state) {halt("[RADIO] setEncoding: ", state);}
  state = radio.setDataShaping(RADIOLIB_SHAPING_NONE);
  if(state) {halt("[RADIO] setDataShaping: ", state);}

  uint16_t counterVal = IOWN_TX_COUNTER;
  uint8_t counter[2] = {(uint8_t)(counterVal >> 8), (uint8_t)(counterVal & 0xff)}; // big-endian in frame

  uint8_t frame[32];
  uint8_t air[48];
  Serial.print(F("[TX] counter=0x"));Serial.println(counterVal, HEX);

  // determine the fixed air length and disable the radio-inserted length byte
  // (real io frames carry no length byte after the sync word; the length lives in ctrl0)
  size_t flen0 = buildFrame(0x00, counter, frame);
  size_t alen  = uartEncode(frame, flen0, air, sizeof(air));
  state = radio.fixedPacketLengthMode(alen);
  if(state) {halt("[RADIO] fixedPacketLengthMode: ", state);}

  // mimic the remote: 4 copies, first with ctrl byte 1 = 0x20
  for(int rep = 0; rep < 4; rep++) {
    size_t flen = buildFrame(rep == 0 ? 0x20 : 0x00, counter, frame);
    uartEncode(frame, flen, air, sizeof(air));
    int16_t st = radio.transmit(air, alen);
    Serial.print(F("[TX] rep "));Serial.print(rep);Serial.print(F(" state "));Serial.println(st);
    delay(40);
  }
  Serial.println(F("[TX] done"));
}

void loop() {delay(1000);}
