# Controlling Somfy io blinds from Home Assistant with a LoRa32

This guide walks through controlling **Somfy io-homecontrol® blinds** from Home
Assistant using a cheap ESP32 **LoRa32** board. It covers how the radio traffic
was captured and decoded, which data you need from your own remote, and how to
build and flash the firmware.

> **Scope and honesty note.** This was reverse-engineered against a *Somfy Situo
> Variation A/M io Pure II* remote and its blind motors. It implements the
> **one-way (1W)** io-homecontrol profile only. It has been verified on that
> hardware; other remotes/motors may differ. The firmware currently *impersonates*
> your existing remote (see [Limitations](#limitations)). Use it only on
> equipment you own.

---

## 1. What this does and how it works

io-homecontrol is an 868 MHz **2-FSK** protocol. A 1W remote (like the Situo)
sends short, authenticated frames: a message with a command, a 16-bit rolling
**counter**, and a 6-byte **MAC** computed with AES from a per-device key.

The LoRa32's SX1276 radio can do the same FSK modulation, so the board can:

1. **Listen** (sniffer firmware) and decode frames to learn your remote's
   address and counter.
2. **Transmit** authenticated frames (tx / mqtt firmware), emulating your remote
   so the motors obey.
3. **Bridge to Home Assistant** over WiFi/MQTT, exposing each blind as a `cover`.

The authentication key is **derivable from the QR code printed on your remote**,
using a transfer key that is already public in this project. No key cracking is
involved — you are using your own remote's credentials.

---

## 2. Hardware

| Item | Notes |
| --- | --- |
| **LilyGO TTGO LoRa32 T3 v1.6.1** | ESP32-PICO-D4 + **SX1276**. Buy the **868 MHz** variant (EU io-homecontrol). The SX1276 silk reads `SX1276/868/915`. |
| 868 MHz SMA antenna | Usually included. |
| USB-C cable | For flashing and power. |

Other boards with an SX127x/SX126x in **FSK** mode at 868 MHz also work; you then
add a matching PlatformIO environment.

> The PlatformIO board id `ttgo-lora32-v21` needs a one-line fix in the `LoRa32`
> support library (a board-macro case mismatch) to compile. This repo's
> `platformio.ini` points `lib_deps` at a local `../LoRa32` checkout carrying
> that fix — use the fork, or apply the fix to upstream `LoRa32`.

---

## 3. Toolchain setup

```bash
# PlatformIO + the Python helper scripts live in one venv
python3 -m venv .venv
.venv/bin/pip install platformio
.venv/bin/pip install kaitaistruct crc   # for the decode scripts
```

The decode/key scripts import modules named `ioCrypto` and `aes`, but the files
in `scripts/` are hyphenated (`Iown-ioCrypto.py`, `Iown-AES.py`) and cannot be
imported under those names. Create import shims once:

```bash
mkdir -p .venv/shims
ln -sf "$PWD/scripts/Iown-ioCrypto.py" .venv/shims/ioCrypto.py
ln -sf "$PWD/scripts/Iown-AES.py"      .venv/shims/aes.py
```

Then run any script with `PYTHONPATH=.venv/shims`.

---

## 4. Firmware environments

`platformio.ini` defines three environments for the board, each driven by its
own source file:

| Environment | Source | Purpose |
| --- | --- | --- |
| `ttgo-lora32-v21-sniffer` | `src/sniffer.cpp` | Receive-only: decode frames over serial |
| `ttgo-lora32-v21-tx` | `src/tx.cpp` | One-shot transmit test (counter/level via build flags) |
| `ttgo-lora32-v21-mqtt` | `src/mqtt.cpp` | The Home Assistant / MQTT integration |

Build/flash a given env, e.g.:

```bash
.venv/bin/pio run -e ttgo-lora32-v21-sniffer -t upload
.venv/bin/pio device monitor -e ttgo-lora32-v21-sniffer   # 115200 baud
```

---

## 5. Listening in (the sniffer)

The sniffer configures the SX1276 for io-homecontrol's physical layer:

- Frequency **868.95 MHz** (io "channel 2"; the Situo uses this for all its
  button-channels)
- **2-FSK**, **38.4 kbps**, **19.2 kHz** deviation, no shaping, NRZ
- Hardware sync word **`0x57FD99`** (the UART-encoded end-of-preamble + `FF 33`)

Over the air the data uses **UART framing** inside the FSK stream: each byte is a
start bit (`0`), 8 data bits **LSB-first**, and a stop bit (`1`). The sniffer
decodes that in software and prints each frame.

Flash it, open the serial monitor, and press a button on your remote near the
board. You'll see lines like:

```
RX rssi=-28.0 raw=... frame=F60000003FB51A3F0001430000000036A94AAEAC756EE08A69 err@25
```

Strong presses read roughly −20 to −70 dBm; anything near −100 dBm is distant
noise from other io devices and can be ignored. `err@n` just marks where the
decoder hit the end of the frame.

### Frame anatomy (1W)

```
F6 00 00003F B51A3F 00 0143000000 00 36A9 4AAEAC756EE0 8A69
└┬ └┬ └──┬── └──┬── └──────┬────── └─┬─ └─────┬────── └─┬
 │  │    │      │          │         │        │        └ CRC (16-bit, poly 0x8408, LSB first)
 │  │    │      │          │         │        └ MAC (6 bytes, AES)
 │  │    │      │          │         └ counter (rolling, big-endian)
 │  │    │      │          └ command + parameters
 │  │    │      └ source address  (your remote channel)
 │  │    └ destination (00 00 3F = broadcast)
 │  └ control byte 1
 └ control byte 0 (0xE0 flags | 5-bit length)
```

- **Address:** bytes 2–4 are the destination, bytes 5–7 the **source** (your
  remote). Each channel of a multi-channel remote is a **separate, adjacent
  address** on the same frequency (e.g. `B51A3F`, `B51A40`, …).
- **Counter:** increments with use. Each physical press sends three frames, so
  the counter jumps by ~3 per press.
- **Commands observed:**
  - `00 01 43 <level> 00 00 00` — go to position: `level 0x00` = open, `0xC8` = close
  - `00 01 43 D2 00 20 D2 00 00` — stop
  - A press also sends two `0x20` frames carrying the button code after `01 43`
    (`00`=up, `01`=down, `02`=My). **"Go to My/favourite" needs the full
    3-frame sequence** (stop + the two `02` frames); the single stop frame only
    halts a moving blind.

> **Alternative without flashing anything:** with an RTL-SDR you can decode io
> frames using [`rtl_433`](https://github.com/merbanan/rtl_433):
> `rtl_433 -R 189 -f 868.95M -s 1000k`.

---

## 6. The data you need from *your* remote

Everything needed to emulate the remote is on the sticker/QR code of the remote
itself, plus what the sniffer shows:

1. **The QR code** on the back of the remote / under the battery cover. For a
   Situo it is **56 hex characters**, laid out as:

   ```
   45 | AABBCC | <32 hex = 16-byte productKey> | <trailer>
        └ address   └ productKey
   ```

   - `qr[2:8]` — the remote's base **address**
   - `qr[8:40]` — the 16-byte **productKey** (the secret this whole thing hinges on)

2. **Each channel's address**, read from the sniffer (one press per channel).
3. **The current counter** per channel — the sniffer shows it; the firmware then
   tracks it automatically.

### Deriving the key

The actual AES key is the productKey "decrypted" with the project's public
**transfer key** and an IV built from the channel address:

```
key = AES(transfer_key, IV_from_address) XOR productKey
```

You don't have to do this by hand. Save the QR text to a file **outside the
repo** (keep it private), then verify and generate:

```bash
# Put your QR string in, say, ~/.config/iown/situo-qr.txt  (chmod 600)

# 1) Confirm the key reproduces the MACs of frames you sniffed:
PYTHONPATH=.venv/shims .venv/bin/python scripts/Iown-VerifyQRKey.py \
    ~/.config/iown/situo-qr.txt captures/<your-frames>.txt
#   -> "productKey decrypted (IV from node ID)  MAC match N/N" means success

# 2) Generate the (git-ignored) key header:
PYTHONPATH=.venv/shims .venv/bin/python scripts/Iown-GenKeyHeader.py \
    ~/.config/iown/situo-qr.txt include/iown_key.h
#   The key is written only to the header and never printed.
```

---

## 7. Configuration files

Two headers are **git-ignored** and must be created locally. Templates are
committed next to them.

### `include/iown_key.h`

Generated by the script in the previous step. Template:
`include/iown_key.example.h`

```c
#pragma once

#define IOWN_NODE_ID     { 0x00, 0x00, 0x00 }        // 3 bytes, frame order (tx.cpp only)
#define IOWN_KEY         { 0x00, /* ...16 bytes... */ } // derived key    (tx.cpp only)
#define IOWN_PRODUCT_KEY { 0x00, /* ...16 bytes... */ } // QR productKey  (mqtt.cpp)
```

### `include/iown_net.h`

WiFi + MQTT credentials, created by hand. Template:
`include/iown_net.example.h`

```c
#pragma once

#define IOWN_WIFI_SSID   "your-ssid"
#define IOWN_WIFI_PASS   "your-wifi-password"

#define IOWN_MQTT_HOST   "192.168.1.10"   // Home Assistant / Mosquitto broker IP
#define IOWN_MQTT_PORT   1883
#define IOWN_MQTT_USER   "mqtt-user"       // broker username ("" if anonymous)
#define IOWN_MQTT_PASS   "mqtt-password"   // broker password ("" if anonymous)
#define IOWN_MQTT_CLIENT "iown-lora32"
```

Copy the templates and fill them in:

```bash
cp include/iown_net.example.h include/iown_net.h   # then edit
# iown_key.h is produced by Iown-GenKeyHeader.py (section 6)
```

> **Keep these private.** `include/iown_key.h`, `include/iown_net.h` and your QR
> text are secrets. They are git-ignored — never commit them, and don't paste the
> productKey anywhere public.

### Declare your blinds

Edit the `BLINDS[]` table at the top of `src/mqtt.cpp` with
each blind's name and the channel address you sniffed:

```c
static Blind BLINDS[] = {
  {"Blind_1", {0xB5,0x1A,0x3F}, 0x3700, {0}}, // name, address, initial counter seed
  {"Blind_2", {0xB5,0x1A,0x40}, 0x0600, {0}},
};
```

Set the seed a little above the highest counter you saw for that channel; it is
only a starting guess and is corrected automatically (section 9).

---

## 8. Home Assistant / MQTT

1. Install the **Mosquitto broker** add-on in Home Assistant and add a login
   (Configuration tab):

   ```yaml
   logins:
     - username: lora32
       password: <pick-a-password>
   anonymous: false
   ```

   Plain username/password on port 1883 is fine on a home LAN; no TLS needed.

2. Put those credentials in `include/iown_net.h`, then build and flash:

   ```bash
   .venv/bin/pio run -e ttgo-lora32-v21-mqtt -t upload
   ```

3. Power the board where it has **both** WiFi coverage and 868 MHz range to the
   blinds.

On boot it runs a crypto self-test, joins WiFi and the broker, and publishes
**Home Assistant MQTT discovery** configs. Each blind appears as a `cover` plus a
"My" button under a single device, no HA YAML required.

### Topics

| Topic | Direction | Payload |
| --- | --- | --- |
| `iown/<name>/set` | command | `OPEN` / `CLOSE` / `STOP` / `MY` |
| `iown/<name>/state` | status | `open` / `closed` (optimistic) |
| `iown/<name>/counter` | status | current rolling counter (retained) |
| `iown/<name>/counter/set` | command | force the counter to a value (resync) |

Test everything with **MQTT Explorer** before relying on HA: watch
`homeassistant/#` for the discovery configs and `iown/#` for state, then publish
`OPEN`/`CLOSE`/`STOP`/`MY` to `iown/<name>/set`.

- **Stop** only visibly halts a blind that is *moving*.
- **My** only goes somewhere if you've programmed a favourite with the physical
  remote (long-press its Stop/My button).

---

## 9. The rolling counter

The motor accepts a frame only if its counter is **ahead** of the last one it
accepted. The firmware keeps each channel's counter in flash (NVS), keyed by
address, and:

- sends one past its stored value on each command, and
- **passively listens** when idle: when it hears your physical remote, it bumps
  the stored counter to match. This **self-heals** the desync caused by using the
  physical remote.

If a blind ever stops responding (counter fell behind), press the physical remote
once (auto-tracking catches up) or publish a higher number to
`iown/<name>/counter/set`.

---

## 10. Limitations

- **Impersonation.** The board transmits using *your remote's* address, so the
  board and the physical remote share one counter at the motor and can push each
  other out of sync. Auto-tracking mitigates it, but the clean fix is to register
  the LoRa32 as its **own** controller (its own address + key, paired via the
  motor's PROG procedure). That is future work.
- **No feedback.** 1W is fire-and-forget; HA cover state is *assumed*, not
  measured. There is no true position reporting.
- **Set-favourite** (long-press) is not implemented — program "My" on the
  physical remote.
- Verified only against the hardware noted at the top.

---

## 11. Security & legal

- Operate only on **your own** equipment.
- The productKey and WiFi/MQTT credentials are secrets; keep them in the
  git-ignored headers and out of any commit, screenshot or paste.
- 868 MHz is licence-free in the EU, but respect local RF regulations and duty
  cycle; these frames are short and infrequent.
