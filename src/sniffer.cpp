/**
 * @file sniffer.cpp "sniffer.cpp"
 * @brief iown-homecontrol receive-only sniffer
 *
 * Listens on io-homecontrol channel 2 (868.95 MHz, 2-FSK, 38.4 kbps, 19.2 kHz deviation).
 * The SX127x packet engine triggers on the UART-encoded end of preamble + "FF 33" start bytes
 * (IOHOME_SYNC_WORD), captures a fixed number of raw bytes and the UART framing
 * (start bit 0, 8 data bits LSB first, stop bit 1) is decoded in software.
 *
 * Output per packet (one line): "RX rssi=<dBm> raw=<hex> frame=<hex> [err@<byte>]"
 * Build/flash: pio run -e ttgo-lora32-v21-sniffer -t upload
 *
 * MIT License
 * Copyright (c) Velocet
 */

#include <Arduino.h>
#include <SPI.h>

#include <LoRa32.h>
#include <RadioLib.h>

#include "IoHome.h"

// 34 byte max frame (ctrl byte + 31 + CRC) * 10 bit = 340 bit -> 43 byte. Round up.
#define SNIFFER_RAW_LEN   (48)
#define SNIFFER_FRAME_MAX (SNIFFER_RAW_LEN * 8 / 10)

LORA32_RADIO radio = new Module(LORA32_SPI_CS, LORA32_RADIO_IO0, LORA32_RADIO_RST, LORA32_RADIO_IO1);

volatile bool received = false;

#if defined(ESP8266) || defined(ESP32)
  ICACHE_RAM_ATTR
#endif
void onReceive() {received = true;}

void halt(const __FlashStringHelper* what, int16_t state) {
  Serial.print(what);Serial.println(state);
  while(true) {delay(1000);}
}

// get bit n (MSB first, as shifted out of the SX127x FIFO)
static inline uint8_t rawBit(const uint8_t* raw, size_t n) {return (raw[n / 8] >> (7 - (n % 8))) & 1;}

// decode UART framed bytes. Returns number of decoded bytes, sets errAt to first framing error (or -1)
size_t decodeUart(const uint8_t* raw, size_t rawLen, uint8_t* out, size_t outMax, int* errAt) {
  size_t bits = rawLen * 8;
  size_t n = 0;
  *errAt = -1;
  for(size_t pos = 0; pos + 10 <= bits && n < outMax; pos += 10) {
    if(rawBit(raw, pos) != 0 || rawBit(raw, pos + 9) != 1) {*errAt = (int)n;break;}
    uint8_t b = 0;
    for(uint8_t i = 0; i < 8; i++) {b |= rawBit(raw, pos + 1 + i) << i;} // LSB first
    out[n++] = b;
  }
  return n;
}

void printHex(const uint8_t* buf, size_t len) {
  for(size_t i = 0; i < len; i++) {if(buf[i] < 0x10) {Serial.print('0');}Serial.print(buf[i], HEX);}
}

void setup() {
  Serial.begin(115200);

  int16_t state = radio.beginFSK(868.95, 38.4, 19.2, 125.0, 10, 16, false);
  if(state) {halt(F("[RADIO] beginFSK: "), state);}

  uint8_t sync[IOHOME_SYNC_WORD_LEN] = {
    (uint8_t)(IOHOME_SYNC_WORD >> 16), (uint8_t)(IOHOME_SYNC_WORD >> 8), (uint8_t)IOHOME_SYNC_WORD};
  state = radio.setSyncWord(sync, IOHOME_SYNC_WORD_LEN);
  if(state) {halt(F("[RADIO] setSyncWord: "), state);}

  state = radio.setCRC(false); // io-homecontrol CRC is inside the UART framed data
  if(state) {halt(F("[RADIO] setCRC: "), state);}
  state = radio.setEncoding(RADIOLIB_ENCODING_NRZ);
  if(state) {halt(F("[RADIO] setEncoding: "), state);}
  state = radio.setDataShaping(RADIOLIB_SHAPING_NONE);
  if(state) {halt(F("[RADIO] setDataShaping: "), state);}
  state = radio.fixedPacketLengthMode(SNIFFER_RAW_LEN);
  if(state) {halt(F("[RADIO] fixedPacketLengthMode: "), state);}

  radio.setPacketReceivedAction(onReceive);
  state = radio.startReceive();
  if(state) {halt(F("[RADIO] startReceive: "), state);}

  Serial.println(F("[SNIFFER] listening on 868.95 MHz"));
}

void loop() {
  if(!received) {return;}
  received = false;

  uint8_t raw[SNIFFER_RAW_LEN];
  float rssi = radio.getRSSI(true, true); // skipReceive: plain getRSSI() restarts Rx in FSK mode and clears the FIFO
  int16_t state = radio.readData(raw, SNIFFER_RAW_LEN);

  if(state == RADIOLIB_ERR_NONE) {
    uint8_t frame[SNIFFER_FRAME_MAX];
    int errAt;
    size_t n = decodeUart(raw, SNIFFER_RAW_LEN, frame, SNIFFER_FRAME_MAX, &errAt);
    // trim to frame length from ctrl byte 0: 1 + (len & 0x1F) + 2 CRC
    if(n > 0) {size_t want = 1 + (frame[0] & 0x1F) + 2;if(want < n) {n = want;}}

    Serial.print(F("RX rssi="));Serial.print(rssi, 1);
    Serial.print(F(" raw="));printHex(raw, SNIFFER_RAW_LEN);
    Serial.print(F(" frame="));printHex(frame, n);
    if(errAt >= 0) {Serial.print(F(" err@"));Serial.print(errAt);}
    Serial.println();
  } else {
    Serial.print(F("[RADIO] readData: "));Serial.println(state);
  }

  radio.startReceive();
}
