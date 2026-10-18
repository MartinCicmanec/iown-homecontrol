// Template for include/iown_key.h (git-ignored). DO NOT put real keys in this
// example file. Generate the real header with:
//
//   PYTHONPATH=.venv/shims .venv/bin/python scripts/Iown-GenKeyHeader.py \
//       <qr-text-file> include/iown_key.h [node-id-hex]
//
// IOWN_PRODUCT_KEY is the 16-byte "productKey" from your remote's QR code
// (qr[8:40]). src/mqtt.cpp derives each channel's AES key from this plus the
// channel address, so this one value covers every channel/blind.
//
// IOWN_NODE_ID / IOWN_KEY are a single channel's address and derived key, used
// only by src/tx.cpp (the one-shot transmit test). src/mqtt.cpp ignores them.
#pragma once

#define IOWN_NODE_ID     { 0x00, 0x00, 0x00 }                                                   // 3 bytes, frame order
#define IOWN_KEY         { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } // 16 bytes
#define IOWN_PRODUCT_KEY { 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00 } // 16 bytes (from QR)
