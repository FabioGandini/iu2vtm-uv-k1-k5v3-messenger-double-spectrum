

# IU2VTM firmware for the UV-K1 and UV-K5 V3


This repository is a fork of the [armel/muzkr UV-K1/K5V3 firmware](https://github.com/armel/uv-k1-k5v3-firmware-custom) (F4HWN, itself derived from Egzumer's UV-K5 firmware), adapted to the UV-K1 / UV-K5 V3 (PY32F071 MCU, BK4829 radio chip).

It adds a text **messenger** (with callsign ID, delivery ACK, range-check ping and optional encryption), a second **spectrum** analyzer with channel-scan, **aircopy** and a few extras. **Every feature is explained simply (ELI5) in the section below.**

**Download:** grab the latest `iu2vtm.bin` from the [Releases](https://github.com/FabioGandini/iu2vtm-uv-k1-k5v3-messenger-double-spectrum/releases) page.

For the base-firmware features (F4HWN menus, scan lists, power settings, etc.) see the [upstream project](https://github.com/armel/uv-k1-k5v3-firmware-custom) and its [wiki/manual](https://github.com/armel/uv-k1-k5v3-firmware-custom/wiki).


# About this fork (feature_messenger branch)

This is the **IU2VTM** build (based on F4HWN/armel v5.6.0). It turns the little
UV-K1 into a tiny text-messaging radio, adds a second kind of spectrum display,
lets you clone radios over the air, and a few quality-of-life extras.

Below, every feature is explained **simply (ELI5 style)** — what it is and how
to use it — followed by the full button map and credits.

> Quick note on radios: most of these features were adapted for the **BK4829**
> radio chip inside the UV-K1, which behaves a bit differently from the BK4819
> in the older UV-K5.

---

## ✉️ 1. Text Messenger — send little text messages (AFSK 1200)

**What it is (simply):** your radio can send and receive short text messages
(up to ~30 characters), like a walkie-talkie that can also "SMS". Two radios on
the **same frequency** with the messenger turned on can chat.

**How to use it:**
- Open the Messenger screen (assign it to a key, or use the programmable-key
  action **MESSENGER**).
- Type with the keypad like an old phone (**T9**): press a number key, and tap
  again to cycle through its letters. To type two letters on the **same** key,
  type the first, **wait ~1 second** (the cursor jumps forward), then type the
  next.
- **MENU** sends the message. The other radio shows it on its screen.

**Important:** for two radios to understand each other they must use the **same
"modulation" (speed)** and the **same frequency**.



## ✅ 3. Delivery confirmation (ACK)

**What it is (simply):** a "read receipt". When you send a message and the other
radio received it, your screen shows a **`+`**. Turn it on with **MsgACK**.

## 🪪 4. Your callsign on every message (stay legal)

**What it is (simply):** ham-radio rules say you must say **who you are** on the
air. This feature automatically sticks your callsign in front of every message,
so a message becomes `IU2VTM01:hello`.

- **Set it on the radio:** in the Messenger screen, **hold DOWN** → the title
  changes to "Set Callsign" → type it → **MENU** to save (**EXIT** cancels). It
  is remembered after a reboot.
- **Or set it from a PC:** in the included CHIRP driver, field "Station callsign
  prefix".
- Leave it empty to turn the prefix off.

## 📡 5. Ping / Range check — "who can hear me, and how far?"

**What it is (simply):** like a sonar "ping". You send a ping, and every other
UV-K1 (with this firmware) that hears it answers automatically. For each answer
you see **who replied**, an estimated **distance**, and the **signal strength**:

```
IU2VTM02 1.5km -95
```

**How to use it:** in the Messenger screen, **hold UP** to send a ping. Replies
appear in the message list.

> ⚠️ **The distance is a rough estimate.** The radio has no GPS, so it guesses
> the distance from the signal strength using a simple radio-physics model.
> Walls, hills, trees and antennas all affect it, so treat it as a ballpark, not
> a GPS reading. It can be calibrated (see `MSG_DIST_REF_RSSI` /
> `MSG_DIST_DB_PER_2X` in `App/app/messenger.c`).
>
> Ping only works **between UV-K1 radios running this firmware** (a stock UV-K5
> doesn't know how to answer).

## 🔒 6. Encryption (ChaCha20) — scramble your messages

**What it is (simply):** locks your messages with a password so only radios with
the **same password** can read them. Turn on with **MsgEnc**; set the password
(**EncKey**) — only via CHIRP — and it must match on both radios.

> ⚠️ **Legal warning:** on **amateur (ham) radio bands, encryption is NOT
> allowed** (you may not hide the meaning of a transmission). Keep encryption
> **OFF** on ham frequencies and only send clear text there. The feature exists
> for study/experiments where it is permitted.

## 📈 7. Two spectrum analyzers (see the band)

**What it is (simply):** a "radio band visualizer" — bars showing where signals
are. This firmware has **two**:

- **F+5** — the F4HWN/Fagci **bandscope** (scans a frequency range).
- **F+7** — the kamilsss655 **spectrum with channel-scan mode**: open it while
  on a **memory channel** and it scans your saved channels instead of a
  frequency range, showing which ones are active. **KEY_4** toggles between all
  channels and your current scan list; **PTT** jumps to the strongest one.

## 🔁 8. Aircopy — clone one radio into another over the air

**What it is (simply):** copy all the settings/channels from one radio to
another **wirelessly**, no cable. Both radios must be close and on the same
frequency.

**How to use it:** **hold the SIDE2 button while turning the radio on** to enter
Aircopy mode. Works between UV-K1 radios (and other radios using the standard
Quansheng aircopy). It is a separate mode and does not interfere with the
messenger.

## 💻 9. CHIRP driver

A modified CHIRP driver (`chirp/iu2vtm.chirp.v5.5.0.messenger.py`) lets you set
the messenger options from a PC: receive on/off (**MsgRX**), ACK (**MsgACK**),
modulation (**MsgMod**), encryption (**MsgEnc**), the password (**EncKey**) and
your **callsign**. Load it in CHIRP, then "Download from radio".

---

## 🎛️ Messenger button map

| Button | What it does |
|---|---|
| **0–9** | type text (T9) |
| **`*`** | switch UPPER / lower / numbers |
| **F** (tap) | delete a character (backspace) |
| **F** (hold) | reset / clear the messenger |
| **UP / DOWN** (tap) | scroll the message history |
| **UP** (hold) | send a **ping** (range check) |
| **DOWN** (hold) | **set your callsign** (then MENU = save, EXIT = cancel) |
| **MENU** | send the message |
| **EXIT** | leave the messenger |

*(Tip: in the messenger, tapping a key does one thing, holding it does another.)*

---

## Building

The `Custom` preset (target `iu2vtm`) is built with the Docker image from the
upstream project:

```bash
./compile-with-docker.sh Custom
# output: build/Custom/iu2vtm.bin
```

Or, with an ARM GCC toolchain installed locally (`gcc-arm-none-eabi`, CMake,
Ninja):

```bash
cmake --preset Custom
cmake --build --preset Custom -j
```

## Flashing

Flash the raw `iu2vtm.bin` from your browser (Chrome/Edge desktop) with
[UVTools2](https://armel.github.io/uvtools2/): put the radio in **DFU mode
(flash mode)**, connect the programming cable and select the `.bin` file.

UVTools2 can also **dump and restore the calibration data** — doing a dump
before flashing anything is strongly recommended.

## Credits

All credit for the messenger, crypto, spectrum and aircopy code goes to their
original authors ([@joaquimorg](https://github.com/joaquimorg),
[@kamilsss655](https://github.com/kamilsss655), [@fagci](https://github.com/fagci),
[@egzumer](https://github.com/egzumer), [@armel](https://github.com/armel)); this
build only adapts and combines them for the UV-K1/PY32 platform and adds the
callsign, ping/range-check and distance-estimate features. Thanks also to
[@muzkr](https://github.com/muzkr), [@OneOfEleven](https://github.com/OneOfEleven)
and [@DualTachyon](https://github.com/DualTachyon), whose work laid the
foundation for the whole UV-K5 open-source ecosystem.

> [!WARNING]
> Use this firmware at your own risk. There is absolutely no guarantee that it
> will work in any way shape or form on your radio(s), it may even brick your
> radio(s), in which case, you'd need to buy another radio.
> Anyway, have fun.

## License

Copyright 2023 Dual Tachyon
https://github.com/DualTachyon

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

    Unless required by applicable law or agreed to in writing, software

    distributed under the License is distributed on an "AS IS" BASIS,
    WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
    See the License for the specific language governing permissions and
    limitations under the License.
