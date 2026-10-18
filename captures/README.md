# Captures

Real io-homecontrol frames captured with the receive-only sniffer (`src/sniffer.cpp`, env `ttgo-lora32-v21-sniffer`).

## situo-variation-am-io-pure-ii_2026-10-18

- Transmitter: Somfy Situo Variation A/M io Pure II (1W remote), node ID `B51A3F`
- Receiver: LilyGO TTGO LoRa32 T3 V1.6.1 (SX1276), 868.95 MHz, 2-FSK, 38.4 kbps, 19.2 kHz deviation
- Button sequence: Up, Stop/My, Down, Up
- `.log`: sniffer serial output (`raw` = SX1276 FIFO after sync word, `frame` = UART decoded frame, `err@n` = first framing error, i.e. end of frame)
- `_frames.txt`: unique decoded frames, one per line, without sync word `FF33`

All 16 unique frames pass the CRC check (`compute_crc_8408`) and decode with the Kaitai parser (`scripts/iohomecontrol.py`).

| Counter | Command | Parameter | Button |
| :-----: | :-----: | :-------- | :----- |
| `36A9` | `0x00` | `014300000000` | Up |
| `36AA`, `36AB` | `0x20` | `02FF0143000C0000`, `02FF01430005FF00` | (after Up) |
| `36AC` | `0x00` | `0143D20020D20000` | Stop/My |
| `36AD`, `36AE` | `0x20` | `02FF0143020C0000`, `02FF01430205FF00` | (after Stop/My) |
| `36AF` | `0x00` | `0143C8000000` | Down |
| `36B0`, `36B1` | `0x20` | `02FF0143010C0000`, `02FF01430105FF00` | (after Down) |
| `36B2` | `0x00` | `014300000000` | Up |
| `36B3`, `36B4` | `0x20` | `02FF0143000C0000`, `02FF01430005FF00` | (after Up) |

Observations (not verified):

- Every command is repeated 3-4 times with the same counter. The first repeat has control byte 1 = `0x20`, the following ones `0x00`.
- Every button press sends one `0x00` (execute function) followed by two `0x20` frames.
