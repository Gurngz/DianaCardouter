# Porting Diana to ESPIDFORTH — cost, space, and design opportunities

> **Status 2026-09-24:** Shape B (embed) is implemented on `pio-migration`:
> `lib/espidforth/` + `src/DianaForth.*` + `src/DianaTune.h`, `/forth`, `/fs`,
> `/tune`, serial REPL, `/diana/boot.fs`. Measured: engine 12.8 KB flash /
> 14.8 KB static RAM at 160 words × 24 chars × 1536 code cells, plus an 8 KB
> data heap; bridge 6.5 KB flash. Image 1,903 KB, static RAM 68.8 KB.

Assessed 2026-09-24 against `../../iotone/ESPIDFORTH` at `dec0591` (v0.5.0) and
this repo's `pio-migration` branch (Arduino core 2.0.17, espressif32 6.13.0).

## What ESPIDFORTH is today

- An **ESP-IDF 5.3.1 component** (`components/forth/forth_core.cpp`, 1441 lines),
  not an Arduino library. Consumers add the component to their own IDF/PIO
  project, call `forth_init`, register C words with
  `forth_register_word(name, void(*)(void))`, and drive it with `forth_eval` or
  `forth_repl`.
- The engine is the **stub**, not ESP32forth. Fixed limits: 512 words, 4096
  code cells, 256-deep stacks, 256-byte input line. No `CREATE`/`DOES>`, no
  floats, no string words beyond `s"`/`."`/`type`, no file words.
- **No peripheral vocabulary.** The core has arithmetic, stack, control flow,
  memory, `chip-*` info and `mem`. There are no WiFi, HTTP, TLS, I2S, I2C, SPI,
  GPIO, SD, NVS, timer, task, or display words. The only FFI example outside the
  core is the trailcam demo (camera + SD, 337 lines of C).
- **PSRAM is assumed on S3.** `sdkconfig.defaults.esp32s3` turns on octal
  SPIRAM and puts the dictionary in `EXT_RAM_BSS`. The Cardputer ADV has none.
  The C3/C6 configs prove the no-PSRAM path works, so this is a config change,
  not a code change.
- Static cost of the engine: ~38 KB dictionary + ~16 KB code array in BSS, plus
  a malloc'd data heap (100 KB in the demo `main.c`). Built S3 image: 217 KB.

## What Diana depends on (the port surface)

| Diana module | Lines | Arduino / M5 dependency | IDF replacement |
|---|---|---|---|
| `DianaUI` | 584 | M5GFX canvases, efont JP | M5GFX has an IDF component: same API |
| `DianaAudio` | 657 | M5Unified `Mic`/`Speaker` (ES8311) | M5Unified IDF component: same API |
| `DianaHttp` | 282 | `WiFiClientSecure` as a `Stream` | `esp_tls` + own chunked reader (rewrite) |
| `DianaGemini` | 593 | `String`, ArduinoJson over the stream | ArduinoJson is plain C++, keep; `String` to `std::string` |
| `DianaNet` | 263 | `WiFi`, `HTTPClient`, `configTime` | `esp_wifi`, `esp_http_client`, `sntp` |
| `DianaSetup` | 190 | `WebServer`, `DNSServer`, `softAP` | `esp_http_server` + captive DNS task |
| `DianaConfig` | 344 | `Preferences`, `SD` | `nvs`, `esp_vfs_fat` + `sdspi` |
| `DianaIR` | 138 | IRremoteESP8266 (`IRsend`, `IRac`) | RMT driver for NEC/Sony/RC5/Samsung; **AC protocol support is lost** unless the 116 KB library is ported |
| `DianaLive` | 289 | hand-written WebSocket over `WiFiClientSecure` | `esp_websocket_client` |
| `main.cpp` | 1215 | `M5Cardputer.Keyboard` (TCA8418 on I2C 8/9) | ~150-line TCA8418 driver |
| Everything | 5800 | `String` in 380 places | `std::string` |

Direct ESP-IDF calls today: three (`esp_random`, `esp_reset_reason`,
`esp_deep_sleep_start`). Everything else goes through Arduino or M5 classes.

## Space check (measured, this branch)

Firmware image: 1,881 KB of a 3,072 KB OTA slot (59.8%), 1,264 KB free.
Static RAM: 53.5 KB of 320 KB.

Flash by archive (from the linker map):

| Archive | Flash KB | Note |
|---|---|---|
| libM5GFX.a | 482 | `lgfx_efont_ja` 220 + `lgfx_efont_cn` 155 + core 107. Only `efontJA_10` is used; the CN table is dragged in by M5GFX's own font list. |
| WiFi stack (net80211, lwip, pp, wpa_supplicant, phy) | 331 | unavoidable |
| mbedtls (mbedcrypto + mbedtls) | 131 | unavoidable |
| libIRremoteESP8266.a | 116 | `IRac` + every AC protocol; Diana sends NEC/Sony/RC5/Samsung plus AC via `IRac` |
| libc + libstdc++ | 104 | |
| Diana `src/` objects | 188 | `main.cpp` 51, `DianaGemini` 26, `DianaConfig` 21, `DianaUI` 15 |
| Arduino layer (FrameworkArduino, WiFi.a, WebServer, HTTPClient) | 84 | this is all a move to bare IDF would remove |

Conclusion: **the port does not buy flash.** Leaving Arduino saves ~85 KB; the
Forth engine adds ~55 KB static plus its heap. Fonts and the IR library are
the real levers and are available without porting (see below).

RAM is the constraint that matters. A no-PSRAM Diana already runs two TLS
sessions at once (Gemini keep-alive plus Live WebSocket, roughly 70–90 KB),
16 KB of mic pre-roll, 14 KB of streaming audio buffers and 44 KB of display
canvases. The Forth demo's 100 KB data heap does not fit next to that; a
16–32 KB heap would.

## Cost estimate

Shape A, **full port** (Diana rewritten on ESP-IDF with ESPIDFORTH as the
scripting layer):

| Work item | Days |
|---|---|
| Board bring-up: IDF project, no-PSRAM S3 sdkconfig, 8 MB partitions, USB-CDC console, M5Unified/M5GFX as IDF components, TCA8418 keyboard driver | 3–5 |
| Streaming HTTPS client + base64 streaming on `esp_tls` (DianaHttp rewrite, the riskiest module) | 3–4 |
| Audio on M5Unified under IDF (same API; the chop/timing tuning in `backups/` shows this took many hardware iterations) | 2–3 |
| UI on M5GFX under IDF | 1–2 |
| Config/NVS/SD/Memory/Tools/Net (`String` to `std::string`, `HTTPClient` to `esp_http_client`) | 3–4 |
| Setup portal on `esp_http_server` + captive DNS | 2 |
| IR on RMT (consumer protocols only) | 1–2 |
| Forth word set (~50 words: speak, listen, ask, tool registry, timers, settings) and reshaping `loop()` as Forth-driven events | 4–6 |
| Hardware validation and regressions | 5–10 |
| **Total** | **25–40 engineering days** |

Shape B, **embed the engine in the existing Arduino build**. **Verified
2026-09-24 by a probe build**: `forth_core.cpp` (v0.5.0, unmodified) compiles
and links inside this project as `lib/espidforth/` with three build flags in
its `library.json`, because Arduino core 2.0.17 sits on ESP-IDF 4.4:

| Gap | Shim |
|---|---|
| `EXT_RAM_BSS_ATTR` does not exist in IDF 4.4 | `-DEXT_RAM_BSS_ATTR=` (it is empty without PSRAM anyway) |
| `CHIP_ESP32C6` enum predates IDF 4.4 | `-DCHIP_ESP32C6=13` |
| Arduino defines `MAX_INPUT` | `#undef MAX_INPUT` before including `forth_core.h` |

Measured cost of the linked engine with a 16 KB data heap and one registered word:

| | Engine | Notes |
|---|---|---|
| Flash | 12.8 KB | image 1,881 → 1,895 KB |
| Static RAM | 57.2 KB | 53.5 → 112 KB static; dictionary 512 × 76 B ≈ 39 KB, code 4096 cells = 16 KB, stacks 2 KB |
| Heap | whatever `forth_init(n)` is given | 16–32 KB is plenty for tuning scripts |

The 57 KB is the one real cost and it is all tunable: the limits are plain
`#define`s in `forth_core.cpp` (`MAX_WORDS 512`, `MAX_WORD_LEN 64`,
`MAX_DICT_CODE 4096`). Guarding them with `#ifndef` upstream (a five-line PR
to ESPIDFORTH) lets Diana build with 128 words × 32-char names and 1024 code
cells for about **11 KB** of static RAM.

Work: register the words that wrap `runTurn`, `speak`, `addTimer`,
`applyDeviceSetting`, the config fields worth tuning (`vad_threshold`,
`silence_ms`, `mic_gain`, `volume`, chunk sizes) and the IR sender; `/forth
<line>` on the HUD and serial console; `run_script` as a Gemini tool; and a
loader that evaluates `/diana/*.fs` from the SD card at boot and on demand.
**2–3 days**, no regression risk to audio or networking.

## What the SD card buys

Flash is not short (1.26 MB free) and the SD card cannot hold executable
code, so it does not change the port-size question. It changes the
*workflow*: Forth source on the card means every tuning constant that is a
`#define` today (`config.h` audio timings, VAD debounce, pre-roll, stream
chunk sizes) can become a variable set from `/diana/tune.fs`, edited on a
laptop and reloaded with one command, with no reflash and no reboot. Signed
role bundles from a hive node would land in the same directory. The engine's
data heap stays small because scripts are read line by line from the card.

Recommendation, given that porting is acceptable: do Shape B now as the
tuning layer, and treat Shape A (bare ESP-IDF) as a separate decision to be
taken only if the engine gains the WiFi/HTTP vocabulary its roadmap lists.
Shape A still costs 25–40 days and buys neither flash nor RAM.

## Design opportunities (independent of the port)

1. **Fonts: 375 KB.** Add an `ascii` build env (`-DDIANA_FONT_ASCII`) that
   never references `efontJA_10`, or generate a subset font (Latin + kana +
   the ~500 most common kanji, ~60 KB) with M5GFX's font converter.
2. **IR: 116 KB.** IRremoteESP8266 honours `-D_IR_ENABLE_DEFAULT_=false` plus
   `-DSEND_NEC=true -DSEND_SONY=true -DSEND_RC5=true -DSEND_SAMSUNG=true` and a
   short list of AC brands. Expect ~80 KB back; the AC tool then only knows the
   brands you list.
3. **Gemini Live is compiled and wired despite the README calling it too
   fragile for this board.** Put `DianaLive` behind `-DDIANA_LIVE=0` by
   default; it removes the second TLS session and its keep-alive from the
   main loop.
4. **Debug console** (`!` commands, `huntStep`, raw ES8311 register writes,
   `main.cpp:1000-1103`) behind `-DDIANA_DEBUG`.
5. **Voice-wake is dead code**: `serviceHandsFree`, `serviceStandbyWake` and
   the filler-clip generator are defined but never called, while the standby
   screen still says "say 'wake up diana'". Decide: finish or delete.
6. **One defaults table.** Config defaults exist three times (member
   initialisers, `loadFromSd`, `loadFromNvs`) and already disagree on `font`.
7. **Split `main.cpp`** along its natural seams: conversation store, device
   settings, timers, speech, turn engine, lifecycle, commands table, input.
8. **Host tests.** The README describes host tests for the base64 streamer and
   the chunked reader, but no `test/` exists. Add `[env:native]` with Unity
   tests once `HttpsStream` takes a `Stream&` instead of owning a
   `WiFiClientSecure`.
9. **Partition table.** `spiffs` (1.9 MB) is unused; nothing includes
   LittleFS. Either use it for the recorded/TTS PCM scratch files (removes the
   SD requirement for voice) or give the space to the OTA slots.
10. **Security** (see `docs/code-quality-sweep.md`): open setup AP that
    pre-fills the WiFi password and API keys, TLS unverified by default, API
    key in the Live URL.
