#!/usr/bin/env python3
"""
Check whether the 16 byte "productKey" from a 1W controller QR code (e.g. Somfy Situo)
is the key used to compute the 1W MAC of captured frames.

The QR payload is read from a file and never printed. Only match results are printed.

Usage: Iown-VerifyQRKey.py <qr payload file> <frames file>
  qr payload file: QR text (56 hex chars), see docs/commands.md "QR Code"
  frames file:     one decoded frame per line (hex, without sync word), e.g. captures/*_frames.txt
"""

import sys

import ioCrypto


def parse_qr(path):
  raw = open(path).read()
  qr = "".join(c for c in raw if c in "0123456789abcdefABCDEF")
  if len(qr) != 56:
    sys.exit(f"QR payload: expected 56 hex chars, got {len(qr)}")
  return bytes.fromhex(qr[2:8]), bytes.fromhex(qr[8:40])


def parse_1w_frame(frame):
  """Split a 1W frame: ctrl0 ctrl1 dest[3] src[3] cmd params.. counter[2] mac[6] crc[2]"""
  return {
    "src": frame[5:8],
    "data": frame[8:-10], # command ID + parameters
    "counter": frame[-10:-8],
    "mac": frame[-8:-2],
  }


def main(args):
  if len(args) != 3:
    sys.exit(__doc__)

  # self test with the documented 0x30 example (docs/linklayer.md)
  doc_key = bytes.fromhex("01020304050607080910111213141516")
  doc_data = bytes.fromhex("307e60491f976adf653db0ed785e49a201")
  assert ioCrypto.create_1W_hmac(bytes.fromhex("1234"), doc_key, doc_data) == bytes.fromhex("19e81ec43d5e"), "self test failed"

  qr_addr, product_key = parse_qr(args[1])
  frames = [bytes.fromhex(l.strip()) for l in open(args[2]) if l.strip()]
  frames = [f for f in frames if f[0] & 0x20 and ioCrypto.compute_crc_8408(f) == 0] # 1W frames with valid CRC
  if not frames:
    sys.exit("no valid 1W frames found")

  node = parse_1w_frame(frames[0])["src"]
  print(f"frames: {len(frames)} valid 1W frames from node {node.hex()}")
  print(f"QR address == node ID:          {qr_addr == node}")
  print(f"QR address == node ID reversed: {qr_addr == node[::-1]}")

  candidates = {
    "productKey as-is": product_key,
    "productKey decrypted (IV from node ID)": bytes(ioCrypto.encrypt_1W_key(node, product_key)),
    "productKey decrypted (IV from reversed node ID)": bytes(ioCrypto.encrypt_1W_key(node[::-1], product_key)),
  }

  for name, key in candidates.items():
    hits = 0
    for f in frames:
      p = parse_1w_frame(f)
      if ioCrypto.create_1W_hmac(p["counter"], key, p["data"]) == p["mac"]:
        hits += 1
    print(f"{name:48s} MAC match {hits}/{len(frames)}")


if __name__ == "__main__":
  main(sys.argv)
