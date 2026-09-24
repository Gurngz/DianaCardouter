# Code quality sweep — 2026-09-24

Scope: `src/` (30 files, 5,800 lines). Build verified after every change with
`pio run` (espressif32 6.13.0, Arduino core 2.0.17). Nothing here has been run
on hardware.

## Fixed on `pio-migration`

| # | Where | Problem | Fix |
|---|---|---|---|
| 1 | `main.cpp` hands-free branch of `loop()` | `return` skipped `serviceTimers()`, so a timer set by voice never fired while listening | `timerDue()` check before the early return; stops listening and falls through when a timer is due |
| 2 | `DianaAudio::streamBegin` / `streamFeed` | 3 × 4.8 KB `malloc` unchecked; `streamFeed` only tested buffer 0, then wrote into 1 and 2 | all three checked; on failure buffers are freed and streaming is disabled |
| 3 | `DianaAudio::streamEnd` | on the 12 s timeout, buffers were freed while the speaker task could still read them | `M5.Speaker.stop(0)` before freeing |
| 4 | `runTurn` tool loop | `for (round < 4)`: the fourth round's reply text was never shown or spoken | loop bounded by `MAX_TOOL_ROUNDS` after the reply is handled; logs "tool round limit reached" |
| 5 | `addTimer` | no upper bound; anything over ~24.8 days wraps `millis()` and fires immediately | rejects outside 1 s … 7 days (`TIMER_MAX_SECONDS`) |
| 6 | `/delkey` | `arg.toInt()` of text is 0, so `/delkey abc` deleted key #0 | requires a numeric argument |

Flash after fixes: 1,881,321 bytes (+552 over the committed 1.0.0 build).

## Open, ranked

### Correctness
- **Setup portal from the menu** (`DianaMenu.cpp:149`) ignores `Setup.start()`'s
  result; `loop()` treats "portal not running" as "saved" and calls
  `ESP.restart()` (`main.cpp:1143`). A failed start reboots the device. The
  menu path also skips the `Gemini.disconnect()` that the `/setup` path does.
- **Config load clobbers settings every boot** (`DianaConfig.cpp:75-76`):
  `silenceMs > 900 → 700` and `micGain > 40 → default`, although `/mic`
  accepts up to 128 and the shipped default `silenceMs` is 1200. These were
  one-off migrations; gate them on a config version field.
- **`volumeOverride` is dead** (`DianaAudio.cpp:479`): `streamPcm` calls
  `speakerMode()`, which resets the volume. Boot music plays at the normal
  volume.
- **History trimming can drop a tool-call turn** (`DianaGemini.cpp:63`) and the
  next `functionResponse` is then rejected by the API. Trim in whole
  user→model→function groups.
- **Serial console runs in every state** (`main.cpp:1107`), so a serial line
  can start a turn while the mic is recording.
- **Every Live failure rotates the API key** and writes NVS (`main.cpp:319`),
  including plain network errors. `DianaLive.cpp:124` also parses the status
  from `"HTTP/1.1 4xx"` with `toInt()`, which always yields 0.
- **WiFi reconnect blocks the UI** for ≥15 s per saved network every 30 s
  when disconnected (`DianaNet.cpp:34,41`, `main.cpp:1210`).
- `spoken.substring(0, 600)` (`main.cpp:270`) can split a UTF-8 sequence.

### RAM (no PSRAM)
- Two TLS sessions coexist by design (Gemini keep-alive + Live WebSocket),
  roughly 70–90 KB. `GeminiLive.end()` is never called, not even in standby.
- Per request: ~25 KB of transient `String` building (`head`, `hist`, the
  4.7 KB `DECLARATIONS` copy in `requestTail()`), allocated during the TLS
  handshake. Write the pieces straight to `_http`.
- `deserializeJson(doc, _http)` (`DianaGemini.cpp:191`) parses the whole
  response including thought signatures; use a `Filter`.
- Display canvases 43.7 KB permanent; `_bar`/`_in` at 8-bit would save 7.4 KB.
  The standby ring (3.9 KB) is never freed.
- `saveConvTurn` re-parses and rewrites the whole history file every turn.

### Dead and duplicated code
- Never called: `serviceHandsFree`, `serviceStandbyWake`, `playThinking`,
  `generateFillersIfMissing`, `genFiller`, `Gemini.tts()`, `finishListening`,
  `stopPlayback`, `isPlaying`, `Setup.saved()`. Settings with no effect:
  `idleSleepSec`, `autoWake`, `voiceWake`; `g_gateWake` is always false.
- The RECORDING branch of `handleKeys` is unreachable (`loop` returns first).
- Duplicated: base64 `"data"` scanner ×3, `jsonEscape` ×2 (different
  control-char handling), WiFi scan summary ×2, UTF-8 backspace ×2, wake-phrase
  check ×3.
- Left-in debugging: the `!` console, `huntStep`, ES8311 register pokes,
  `g_freezeUI`, `bootTest` at volume 255 (bypasses the 170 brown-out cap).

### Structure
- `main.cpp` seams: conversation store (74-114), device settings (116-185),
  timers, speech (212-353), turn engine (355-518), lifecycle (520-600),
  commands table (602-715), input (717-868), debug console (1000-1103).
- App state is spread over `UI.state()`, `awake`, `Menu.isOpen`,
  `Music.isOpen`, `Audio.isListening`, `shutdownArmed`.
- Defaults live in three places and disagree (`font`: `"jp"` in the header,
  `"ascii"` in both loaders). `"Leda"`, the TTS model name, `D-I-0336-7` and
  the Gemini host are each hard-coded a second time.
- Globals named `Audio`, `Config`, `Setup`, `Net` are collision-prone.

### Security
- Setup AP `DIANA-SETUP` is open, `/save` is unauthenticated, and the form
  pre-fills the stored WiFi password and every API key (`DianaSetup.cpp:62-65`).
  At minimum: WPA2 with a printed one-time PSK, and never echo secrets.
- TLS is `setInsecure()` by default; Live ignores `ca.pem` and sends the API
  key in the URL query string.
- `[CONSOLE] >` echoes `/key` and `/wifi` lines to serial.
- `voice`, `langCode` (Live setup JSON) and `chatModel` (URL path) are user
  input inserted without escaping.
- The `connect_wifi` tool lets the model change networks; reachable through
  prompt injection from search results.
