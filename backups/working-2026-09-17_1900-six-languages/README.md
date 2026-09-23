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

Skipped step 3? Diana opens a **setup portal**: join WiFi `DIANA-SETUP` from your phone,
open `http://192.168.4.1`, fill in the form, Save. (`/setup` from the chat reopens it any time.)

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

## Build it yourself

Requires PlatformIO (`pip install platformio`). The espressif32 platform + Arduino core 2.0.17 are used.

```bash
pio run                          # → .pio/build/cardputer-adv/firmware.bin
pio run -t upload                # flash over USB-C instead of the launcher
python tools/make_boot_wav.py    # theme MP3 → sdcard/diana/boot.wav (16 kHz mono, 14 s)
python tools/package_sd.py       # build + sdcard/Diana.bin + build/*.bin + build/Diana-SD-<ver>.zip
```

`build/Diana-cardputer-adv-<ver>-full.bin` is a merged image for `esptool.py write_flash 0x0 …` or web flashers.

### Layout

```
src/
  main.cpp          state machine: boot → standby → welcome → chat; keys; slash commands; timers
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
