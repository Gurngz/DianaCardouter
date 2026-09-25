# 🤖 DIANA (D-I-0336-7) — Cardputer ADV edition

The pocket body of [Diana](../Diana), the PC voice assistant. Same persona, same
welcome protocol, same long-term memory format — squeezed into an M5Stack
**Cardputer ADV** (ESP32-S3, 8 MB flash, no PSRAM, 1.14" screen, 56 keys,
mic, speaker, microSD, WiFi). Installs from the SD card through M5Launcher.

```
┌─────────────────────────────────────────┐
│ ◉ DIANA  LISTENING        18:44  87%  ▮▮▮▮│
│ # DIANA awakened.                        │
│ Diana fully functional. Good evening.    │
│ It's clear skies and 21 degrees in ...   │
│ > what's the weather in tokyo            │
│ # tool: weather_report                   │
│ Tokyo is partly cloudy at 24 degrees     │
│ right now, with a light breeze.          │
│─────────────────────────────────────────│
│ > _                     type or TAB=talk │
└─────────────────────────────────────────┘
```

## What works on the Cardputer (and what doesn't)

| Feature | PC Diana | Cardputer Diana |
|---|---|---|
| Persona / core protocol | ✅ | ✅ same prompt, adapted for a pocket body |
| Text chat | ✅ | ✅ keyboard → `gemini-3.5-flash-lite` (configurable) |
| Voice input | ✅ Gemini Live | ✅ push-to-talk: mic → 16 kHz PCM → Gemini audio understanding; Diana echoes what she heard |
| Voice output | ✅ Live audio | ✅ Gemini TTS (24 kHz PCM) streamed to SD, played on the ES8311 speaker |
| Wake word | ✅ "Diana wake up" | ⚠️ any key wakes (no on-device wake-word model without PSRAM) |
| Welcome protocol | music → "Diana fully functional" → weather | ✅ chirp → boot music from SD → greeting + live weather |
| Long-term memory | `memory/long_term.json` | ✅ `diana/memory.json`, identical schema; `save_memory` / `forget_memory` tools |
| Tools | 30+ | ✅ weather, timers, notes, device status/settings, web search*, mute, Barnyard shutdown |
| Vision / face / webcam | ✅ | ❌ no camera |
| PC control, browser, apps | ✅ | ❌ (Diana says so and points you to the PC body) |
| Discord / Telegram | ✅ | ❌ |
| Real-time streaming (Gemini Live) | ✅ | ❌ too fragile with 8 MB / no PSRAM; turn-based instead |

\* Web search uses Gemini's Google-Search grounding. On the free API tier it returns quota errors (429) — Diana reports that gracefully.

Typical voice turn: ~1.5 s TLS + 1–3 s model + 2–4 s TTS ≈ 5–8 s. Text turns without voice are ~2–4 s.

## Install from the SD card (no cable)

1. Flash **M5Launcher** once over USB — `Launcher-m5stack-cardputer.bin` from
   <https://github.com/bmorcelli/M5Stick-Launcher/releases> (one build covers Cardputer and Cardputer ADV).
   Use their web flasher or M5Burner.
2. Format a microSD as **FAT32** (≤ 32 GB recommended) and copy everything from
   [`sdcard/`](sdcard/) to the card's root: `Diana.bin`, `README.txt`, `diana/`.
3. Edit `diana/config.json`: WiFi name/password and your Gemini API key
   (<https://aistudio.google.com/apikey> — the same key as `Diana/config/api_keys.json` works).
   Optional: `user_name`, `tts_voice`, `timezone` (POSIX, e.g. `JST-9`), `font: "jp"` for Japanese glyphs.
4. Insert the card, power on, press **ENTER** on the launcher screen → **OTA → SD card → `Diana.bin`** → install.
5. Diana boots into standby. Press any key: welcome protocol runs, then chat.

Skipped step 3? Diana opens a **setup portal**: join WiFi `DIANA-SETUP` from your phone (the WPA2
password `diana-xxxx` is printed on Diana's screen), open `http://192.168.4.1`, fill in the form, Save.
(`/setup` from the chat reopens it any time. The form never shows stored secrets; leave a field blank to keep it.)

Optional: copy the PC's `Diana/memory/long_term.json` to `diana/memory.json` so she already knows you.

## Using Diana

| Key | Action |
|---|---|
| type + **ENTER** | send text |
| **TAB** | start a voice message; TAB/ENTER sends, or it auto-sends after 1.5 s of silence |
| **Fn + ;** / **Fn + .** | scroll the log |
| **Fn + `** (ESC) | clear input · stop speaking · go to standby |
| `/help` | all slash commands |

Slash commands: `/setup` `/wifi <ssid> <pass>` `/key <apikey>` `/name <you>` `/voice on|off|Leda`
`/mute` `/unmute` `/vol 0-255` `/bright 0-255` `/memory` `/forget cat/key` `/notes` `/clear`
`/status` `/model <id>` `/ttsmodel <id>` `/think minimal|low|` `/sleep` `/reboot` `/off`

Say **"Barnyard Protocol"** or **"Goodnight Diana"** → farewell → power off.

## Forth scripting (tuning without a reflash)

Diana embeds [ESPIDFORTH](https://github.com/IoTone/ESPIDFORTH) (vendored in `lib/espidforth/`,
about 15 KB of RAM and 20 KB of flash). Every audio and conversation constant worth adjusting is a
named tunable, and behaviour can be scripted from the SD card:

| Where | What |
|---|---|
| `/forth <code>` | evaluate one line on the HUD, e.g. `/forth s" vad" 700 tune!` |
| `/tune` · `/tune vad` | list all tunables with ranges, or read one |
| `/fs <name>` | run `/diana/<name>.fs` from the card (`/fs boot` re-runs the boot script) |
| `diana/boot.fs` | runs at every boot after config and audio are up (sample in `sdcard/diana/`) |
| serial: `forth` … `bye` | REPL over USB at 115200 baud |

Vocabulary (`/forth diana-words`): `say` `ask` `log` `( addr u -- )`, `set ( name val -- )` for any
device setting, `tune@` / `tune!` / `tunes` / `cfg-save`, `timer ( secs label -- )`, `ir ( addr cmd proto -- )`,
`ir-run`, `tone ( hz ms -- )`, `beep`, `mute`, `sleep`, `wake`, `status`, `heap`, `ms`, `wait`, `wifi?` `sd?` `awake?`,
`include ( name -- )`. Tunables: `vad` `silence_ms` `mic_gain` `volume` `brightness` `stream_chunk` `stream_prebuf`
`reply_tokens` `rec_max_sec` `history_turns` `history_chars`. Strings are `s" text"`; lines are limited to
250 characters; `\` starts a comment. The engine is ESPIDFORTH's core word set (`words` lists it).

## Build it yourself

### Prerequisites

- Python 3 and PlatformIO: `pipx install platformio` (or `pip install platformio`). The VS Code
  PlatformIO extension works too; `.vscode/extensions.json` recommends it.
- Nothing else to install by hand. The first `pio run` downloads the pinned toolchain
  (`espressif32@6.13.0`, Arduino core 2.0.17) and the libraries in `platformio.ini`
  (M5Unified, M5GFX, M5Cardputer 1.2.0, ArduinoJson 7, IRremoteESP8266) into `.pio/`.
- Keep the platform pinned. An unpinned `espressif32` can resolve to a pioarduino core 3.x install
  from another project and fail with `Network.h: No such file or directory`.

### Build

```bash
pio run                                  # -> .pio/build/cardputer-adv/firmware.bin (about 1.9 MB)
pio run -e cardputer-adv -t clean        # start over
PLATFORMIO_BUILD_FLAGS="-DDIANA_DEBUG_CONSOLE=0" pio run   # without the '!' codec-register serial commands
```

The linker map lands in `.pio/build/cardputer-adv/firmware.map`; `docs/espidforth-port-assessment.md`
has a per-library flash/RAM breakdown from it. Current figures: 60% of the 3 MB OTA slot, 69 KB static RAM.

### Package for the SD card

```bash
python tools/package_sd.py               # builds, then writes:
#   sdcard/Diana.bin                        app image for M5Launcher
#   build/Diana-cardputer-adv-<ver>.bin     same app image
#   build/Diana-cardputer-adv-<ver>-full.bin  merged bootloader+partitions+app for esptool / web flashers at 0x0
#   build/Diana-SD-<ver>.zip                the sdcard/ folder zipped
python tools/package_sd.py --no-build    # package the last build
python tools/make_boot_wav.py            # theme MP3 -> sdcard/diana/boot.wav (16 kHz mono, 14 s)
```

The version comes from `DIANA_VERSION` in `platformio.ini`. The merged image is built with
PlatformIO's own Python (esptool's dependencies live there); if that step fails the SD package is
still produced.

### Install

| Method | Command / steps | Keeps M5Launcher? |
|---|---|---|
| **SD card via M5Launcher** (normal) | copy `sdcard/*` to the card root, Launcher → OTA → SD card → `Diana.bin` | yes |
| **USB into the launcher's app slot** (what the launcher does, from a cable) | `~/.platformio/penv/bin/python ~/.platformio/packages/tool-esptoolpy/esptool.py --chip esp32s3 --port /dev/cu.usbmodem101 --baud 921600 write_flash 0x170000 .pio/build/cardputer-adv/firmware.bin` | yes |
| **USB full flash** (no launcher) | `pio run -t upload`, or esptool `write_flash 0x0 build/Diana-cardputer-adv-<ver>-full.bin` | **no** (replaces bootloader + partitions; reflash the launcher to get it back) |

The `0x170000` offset is where M5Launcher keeps the installed app (`ota_0` in its partition table);
check it on your unit before trusting it: `esptool.py --port ... read_flash 0x8000 0xc00 pt.bin`.
Find the port with `pio device list` (the Cardputer ADV shows as "USB JTAG/serial debug unit").

### Watch it run

```bash
pio device monitor                       # 115200 baud; every boot step, tool call, HTTP status and heap figure
```

Over the same serial link: `/command`s work, plain text talks to her, `forth` … `bye` opens the Forth
REPL (see "Forth scripting" above). A fresh device with no `diana/config.json` boots into the setup
portal; its WPA2 password is on the screen.

### Layout

```
src/
  main.cpp          state machine: boot → standby → welcome → chat; keys; turn engine; timers
  DianaApp.h        the state and entry points main.cpp shares with the two files below
  DianaCommands.*   slash commands (/setup /wifi /key /voice ... /help)
  DianaConsole.*    USB serial console; `-DDIANA_DEBUG_CONSOLE=0` strips the `!` codec-register commands
  DianaJson.h       shared JSON string escaping
  DianaForth.*      ESPIDFORTH bridge: Diana vocabulary, tunables table, /forth /fs, boot.fs
  DianaTune.h       runtime tunables (defaults from config.h)
lib/espidforth/     vendored ESPIDFORTH core (see its README for the three build shims)
  prompt.h          DIANA CORE PROTOCOL (Cardputer edition of core/prompt.txt)
  DianaGemini.*     REST client: chat + function calling (thoughtSignature-safe), TTS streaming, grounded search
  DianaHttp.*       streaming HTTPS (chunked bodies, keep-alive, body exposed as a Stream)
  DianaAudio.*      mic → SD PCM with silence detection; PCM/WAV playback; chirps
  DianaUI.*         cyan blueprint HUD: status bar, wrapped scrolling log, input strip, standby/boot/setup scenes
  DianaMemory.*     long_term.json-compatible memory with trimming
  DianaConfig.*     config.json on SD + NVS mirror
  DianaNet.*        WiFi, NTP (offset from ip-api), open-meteo weather/geocoding
  DianaSetup.*      AP + captive web form for credentials
  DianaTools.*      tool declarations + executors
  Base64Stream.h    streaming base64 (audio upload / TTS download without big buffers)
tools/              package_sd.py, make_boot_wav.py
sdcard/             what goes on the card
partitions/         8 MB table used for USB flashing (the launcher manages its own)
```

### Design notes for the hardware limits

- **No PSRAM** → audio never sits in RAM: the mic writes 32 ms chunks to `/diana/.rec.pcm`, the upload
  base64-encodes straight from the file, the TTS reply is base64-decoded straight to `/diana/.tts.pcm`.
  Voice therefore needs the SD card; text chat works without it (config/memory fall back to NVS).
- Gemini 3.x models require the `thoughtSignature` to be echoed on tool calls — the model turn is stored
  verbatim in history for tool rounds. `thinking_level: "minimal"` keeps replies fast.
- The HUD uses an 8-bit canvas for the log and 16-bit strips for the bars (~40 KB total).
- TLS uses `setInsecure()` by default; drop a `diana/ca.pem` (Google Trust Services root) on the card to verify.

## Verified while building (2026-09-15)

- Cardputer ADV = ESP32-S3FN8 (8 MB flash, no PSRAM), ES8311 codec, TCA8418 keyboard on I²C 8/9, SD on SPI 40/39/14/12
- M5Cardputer 1.2.0 auto-selects the ADV keyboard reader; M5Unified 0.2.22 drives the ES8311
- M5Launcher 2.9.1 installs app `.bin` files from SD on Cardputer + ADV
- `gemini-2.5-flash` is retired for new keys; `gemini-3.5-flash-lite` handles text, tools and raw `audio/L16;rate=16000` input
- TTS via `generateContent` on `gemini-2.5-flash-preview-tts` / `gemini-3.1-flash-tts-preview` returns 24 kHz PCM
- Host tests: streaming base64 + TTS scanner reproduce the PCM byte-for-byte; chunked HTTP reader passes keep-alive/close cases

Not yet run on real hardware. If something misbehaves, plug in USB-C and watch the serial log
(`pio device monitor`, 115200 baud) — every boot step, tool call, HTTP status and heap figure is printed there.
