# Code quality sweep — 2026-09-24

Scope: `src/` (5,800 lines at the start). Every change was build-verified with
`pio run` (espressif32 6.13.0, Arduino core 2.0.17), in both the default and
the `-DDIANA_DEBUG_CONSOLE=0` variants. **Nothing here has been run on
hardware**; the audio and WiFi changes in particular deserve a bench pass.

## Fixed on `pio-migration`

### Correctness
| Where | Problem | Fix |
|---|---|---|
| `loop()` hands-free branch | `return` skipped `serviceTimers()`; voice-set timers never fired while listening | `timerDue()` before the early return; stops listening and falls through when one is due |
| `DianaAudio::streamBegin/streamFeed` | 3 × 4.8 KB `malloc` unchecked; only buffer 0 tested before writing into 1 and 2 | all three checked; on failure buffers freed and streaming disabled |
| `DianaAudio::streamEnd` | on the 12 s timeout, buffers freed while the speaker task could still read them | `M5.Speaker.stop(0)` before freeing |
| `DianaAudio::playWavFile` | `volumeOverride` was overwritten by `speakerMode()` inside `streamPcm`; boot music ignored its 3/4 volume | override goes through `_volume`, restored afterwards |
| `runTurn` | the fourth tool round's reply text was dropped | loop bounded by `MAX_TOOL_ROUNDS` after the reply is handled |
| `addTimer` | no upper bound; > 24.8 days wraps `millis()` and fires at once | 1 s … 7 days (`TIMER_MAX_SECONDS`) |
| `/delkey` | `toInt()` of text is 0, so `/delkey abc` deleted key #0 | numeric argument required |
| `DianaConfig::load` | `silenceMs > 900 → 700` and `micGain > 40 → default` ran on every boot, undoing `/mic` and the shipped `silence_ms: 1500` | migrations gated on `config_version` (`CONFIG_VERSION` 2), stored in SD JSON and NVS |
| Menu → setup portal | `Setup.start()` result ignored; `loop()` treated "portal not running" as "saved" and rebooted; Gemini/Live sockets left open | menu raises a request, `openSetupPortal()` drops both TLS sockets first and reports failure; `loop()` only reboots when `Setup.saved()` |
| `DianaGemini::trimHistory` | a single oversized tool-call turn could erase itself, orphaning the next `functionResponse` | never trims the newest entry |
| `speak()` Live fallback | every Live failure (including network) rotated the API key and wrote NVS | rotate only on 401/403/429 |
| `DianaLive::openSession` | HTTP status parsed with `toInt()` on `"HTTP/1.1 4xx"` → always 0 | parses the code after the first space |
| `DianaNet::connect` | 15 s floor plus a scan on every call; the 30 s background retry froze the HUD | floor and scan only on the boot path (progress callback given) |
| `speak()` / `saveConvTurn` | `substring()` could split a UTF-8 glyph, producing invalid JSON | `utf8Truncate()` |
| Serial console | a serial line could start a turn while the mic owned I2S | stops recording/listening before `runTurn` |

### RAM
- `enterStandby()` now calls `GeminiLive.end()`, so only one TLS socket survives sleep.
- The standby pulse canvas (3.9 KB) is freed when the scene changes.

### Security
- Setup AP is WPA2 with a per-device key (`diana-` + last MAC bytes), shown on the screen (`SETUP_AP_PSK_PREFIX`).
- The portal form no longer echoes the stored WiFi password or API keys; blank fields keep the stored values.
- `/key`, `/addkey` and `/wifi` are masked in the serial echo.
- Live setup JSON escapes `voice` and `languageCode`; `/model` and `/ttsmodel` accept only URL-safe ids.
- The Live WebSocket honours `/diana/ca.pem` like the REST client (still `setInsecure()` when absent).

### Dead code, duplicates, structure
- Removed (never called): `serviceHandsFree`, `serviceStandbyWake`, the filler-clip generator (`genFiller`, `generateFillersIfMissing`, `playThinking`), `Gemini::tts()`, `Audio::finishListening/recordedSeconds/stopPlayback/isPlaying`, the unreachable RECORDING branch in `handleKeys`, the always-false wake-word gate (`g_gateWake`, `g_turnSuppressed`, `lastInteractionMs`, `FOLLOWUP_MS`), `g_listenPaused`, `idleSinceMs`, `ttsAvailable` (always equal to `sdOk`), `TTS_PATH`, `DIANA_NAME`.
- Standby screen text now matches reality (typed wake only).
- Shared helpers: `DianaJson.h` (one `jsonEscape`; the Live copy had different control-char handling), `utf8Truncate`, `utf8Backspace`, `isWakePhrase` (was 3 copies), `parseIrArgs` (was 2 copies).
- Defaults in one place: `DEFAULT_*` in `config.h` used by the header, the SD loader and the NVS loader (they disagreed on `font`). `LIVE_MODEL`, `DEFAULT_TTS_VOICE`, `GEMINI_HOST`, `DIANA_ID` replace their hard-coded twins.
- `main.cpp` 1,215 → 855 lines: slash commands in `DianaCommands.cpp`, serial console in `DianaConsole.cpp`, shared state declared in `DianaApp.h`. The `!` register-poke commands and the chop hunt compile out with `-DDIANA_DEBUG_CONSOLE=0` (−3.9 KB).

Flash: 1,881 KB (unchanged within 1 KB). Static RAM: 53.5 KB.

## Still open

- **Two TLS sessions while awake** (Gemini keep-alive + Live WebSocket, ~70–90 KB). Make Live a build option (`-DDIANA_LIVE=0`) if the REST path is good enough.
- **Per-request `String` building** (~25 KB transient: system prompt + history + the 4.7 KB `DECLARATIONS` copy) right as the TLS handshake runs. Write the pieces straight to the socket.
- `deserializeJson(doc, _http)` parses the whole reply; a `Filter` would skip thought signatures and grounding metadata.
- `_bar`/`_in` canvases at 16-bit (7.4 KB saving at 8-bit); `saveConvTurn` rewrites the whole history file each turn; `readNotes` reads byte-by-byte into a `String`.
- The base64 `"data"` scanner still exists twice (`ttsStream`, Live `speak`); the Live one meters peak level inline so they were not merged.
- Settings that are stored but do nothing: `idle_sleep_sec`, `auto_wake`, `voice_wake`. Either implement or drop them from the menu and tools.
- TLS is still unverified without `ca.pem`. Embedding the Google Trust Services roots is the fix; it needs a bench test because a wrong chain takes the device offline.
- The Live API key travels in the WebSocket URL (Google's documented form); `x-goog-api-key` may work and would keep it out of logs.
- The `connect_wifi` tool is reachable through prompt injection from search results.
- Menu saves with `Config.save(true)` regardless of SD presence (harmless: the SD write just fails).
- Further `main.cpp` seams if wanted: turn engine (`runTurn`, `speak`, mood/lang), lifecycle, conversation store.
