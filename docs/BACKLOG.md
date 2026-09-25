# Development backlog

Working list for the `develop` branch. Newest decisions at the top of each item;
move an item to **Done** with its commit when it lands.

## Next up

1. **Skills audit + HTML manual test plan.**
   Inventory everything Diana can do on this device and write a device test plan
   in the DianaXR format, so a bench pass is a checklist instead of memory.
   - Audit into `docs/SKILLS.md` (modelled on `../DianaXR/docs/SKILLS.md`):
     every capability, where the answer comes from (local, device tool, model
     tool, network API), what it needs (WiFi, SD, API key), and its state.
     Surfaces to cover: the 25 model tools in `DianaTools.cpp`
     (`save_memory` … `shutdown_diana`), the 43 slash commands in
     `DianaCommands.cpp`, the Forth vocabulary and tunables in `DianaForth.cpp`,
     keys, the setup portal, and the settings menu.
   - Flag the dead or inert ones found in the quality sweep (`idle_sleep`,
     `auto_wake`, `voice_wake`), tools with no local fallback, and anything the
     model can reach that it should not (`connect_wifi`).
   - Test plan: `docs/manual-test-plan.md` as the source, rendered to
     `docs/manual-test-plan.html` by a copy of
     `../DianaXR/scripts/build-test-plan.py` (same Markdown conventions:
     frontmatter, `## Section {class: label}`, `| say | expected | skill |` rows,
     `>` callouts; output is self-contained, checkable, progress saved in the
     browser, light/dark aware).
   - Pass/fail signal is the serial log (`pio device monitor`), not the spoken
     reply, the same rule DianaXR uses with logcat. Rows can be driven over USB
     with plain text, `/commands`, or `forth` … `bye`.
   - Seed sections from what exists: first boot and setup portal, keys and
     input, text turn, push-to-talk and hands-free voice, each tool, timers,
     memory and history, IR, music, settings menu, Forth and tunables, audio
     (the `codec_hold` pop A/B), shutdown, and the fixes from
     `docs/code-quality-sweep.md` that still need hardware confirmation.
   - Note: `../diana-ai` does not exist in this tree. DianaXR is the only
     project with this format today (`DianaXR/diana-ai-axr` and `../Diana`
     have no test plan).

2. **Runtime voice and model selection.** Today: `/voice <name>`, `/model <id>`,
   `/ttsmodel <id>`, `/think`, and the setup portal (voice dropdown, chat model
   field) all change settings at runtime and persist. Gaps:
   - The **Live model** (Diana's main voice path) is the compile-time
     `LIVE_MODEL`; make it `Config.liveModel` with `/livemodel <id>` and a
     portal field, falling back to the constant when empty.
   - **No discovery or validation:** one shared voice table (today it lives only
     in `DianaSetup.cpp`) feeding `/voices`, the portal and the menu; warn on an
     unknown voice instead of silently falling back to REST TTS.
   - **Settings menu:** add Voice (cycle the table, speak a sample) and Model
     (short preset list plus "custom").
   - **Forth:** `set` covers them (`s" voice" s" Kore" set`), plus `voice?` /
     `model?` readers.
   - Keep the model's `device_settings` tool **unable** to change models:
     a prompt-injected bad id would take Diana offline.

3. **Personas.** Source of truth: **RobotAR.me** (IoTone Japan). Persona
   configurations and rules live there; the build fetches them and bakes them
   into the firmware. The free edition ships a baked-in set; paid editions get a
   premium setup with more personas and features. For now the project and the
   free set are open source. Adopt the DianaXR persona system: one spec document per
   character, each with a named owner, and the device builds the prompt from the
   spec instead of hard-coding it. Format and governance come from
   `../DianaXR/personas/README.md`; `../DianaXR/personas/diana.md` is the worked
   example. The nine spec sections are ownership, canon, identity, what they
   feel, how they speak, voice delivery, restraints, failure and honesty, and
   open change requests. Rule 1: nobody edits a persona in code. Rule 2: a
   change needs the owner's sign-off in the changelog.
   - **Split CHARACTER from CAPABILITY.** Today `src/prompt.h` (2.6 KB) mixes
     Diana's character with Cardputer capability rules. Character also leaks
     into code:
     - `moodToStyle()` in `main.cpp` hard-codes "young android" style prompts.
     - `DEFAULT_TTS_STYLE` says "peppy young voice", and `DEFAULT_TTS_VOICE` is Leda.
     - The greeting "I am Diana, D-I-0336-7" is fixed.
     - The midnight whisper and the "Barnyard Protocol" farewell are in code.
     All of these become persona fields. What stays engineering-owned and
     shared by every persona: tools, memory, the Cardputer's limits, languages,
     and the TTS-engine instruction in `DianaLive.cpp`.
   - **Bind voice to persona.** Each spec names its default Live voice, TTS
     style prefix, mood-to-style table, and optional per-language voice. The
     user's `/voice` still overrides. Candidate voices must be auditioned on the
     device, not picked from descriptions. This builds on item 2.
   - **Build-time fetch from RobotAR.me.** A pre-build step (PlatformIO
     `extra_scripts`, e.g. `tools/fetch_personas.py`) pulls the persona set for
     the edition being built and generates a header the firmware compiles in.
     - **Pin and cache:** a lockfile records the persona set version and
       checksums; the fetched copy is cached so builds are reproducible and work
       offline. A changed checksum fails the build instead of silently shipping
       different characters.
     - **Editions:** this repo builds the free edition (`cardputer-adv`,
       public persona set). The premium edition is a **separate closed-source
       repo that depends on this one** (as a submodule or PlatformIO library),
       adds its own personas and features, and defines its own build
       environment. Nothing premium lives here, not even an env stub.
     - **Budget:** each character block goes into every request, so cap it at
       about 2.5 KB (the current Diana prompt is the reference). The fetch step
       enforces it.
     - **Selection:** `/persona <name>`, the settings menu, and the setup portal
       choose among the baked-in personas; never a model tool. Memory and chat
       history per persona or shared is still to decide; Jeeves should not
       inherit Diana's "we talked about…".
   - **Needs from RobotAR.me before the build step can exist:** an endpoint or
     repo path, a persona file format (DianaXR's nine-section Markdown, or a
     structured form of it with the voice bindings as fields), versioning, and
     how the premium set is authenticated (used only by the premium repo).
   - **Open source vs premium: decided 2026-09-25.** Premium is closed source
     in its own repo and depends on this one. What that asks of this repo is
     a clean seam, not premium code:
     - a persona registry the build fills from generated code, so another repo
       can supply more personas without patching ours;
     - feature hooks (extra tools, commands, Forth words, menu items) that a
       dependent repo can register at startup, with this repo's defaults
       working when nothing registers;
     - a stable public header set for those seams, and a note in the README
       that it is the extension API, so changes to it are deliberate.
     The premium repo holds its own credential for fetching the paid persona
     set from RobotAR.me.
   - **Roster**, each needing an owner before it ships:

     | Persona | Status | Notes |
     |---|---|---|
     | **Diana** | exists (DianaXR v8.0, owner Dipen) | Moves to RobotAR.me as a derivative of DianaXR `diana.md`, not a fork. DianaXR should read the same copy eventually. |
     | **Jeeves** | concept, not fleshed out | The premium, more serious persona in the DianaXR README. Needs canon, restraints, and a voice. |
     | **HAL 9000-inspired** | idea | Calm, even, unfailingly polite, and precise. Restraints must rule out the menace and refusal that define the original. |
     | **Cortana-inspired** | idea | Capable, dry humor, a partner rather than a servant. |
     | **Japanese-language character** | idea | Native Japanese register (keigo choices, sentence-final particles, reactions) rather than a translated persona. Pairs with `ja-JP` Live language, the efont JA glyphs already in flash, and a voice auditioned for Japanese. |

   - ⚠️ **Names and IP.** HAL 9000 and Cortana are other companies'
     characters. Ship original characters *inspired by* them, with their own
     names and lines. Keep the inspiration in the canon section of the spec,
     never in user-facing text or the prompt. Pragmata's Diana is an existing
     product decision, outside this item.
   - Output, in order: agree the RobotAR.me format with IoTone Japan; the
     fetch step with lockfile, cache and size check; the code change to
     `Prompt = character (baked) + capability + live` with the persona registry
     and feature hooks as the extension seam; Diana transcribed as the
     first entry in RobotAR.me (a derivative of DianaXR `diana.md`, not a fork);
     stub specs for the others with §9 open questions for their owners.

4. **Make Gemini Live a build option** (`-DDIANA_LIVE=0`). Drops the second TLS
   session (~70–90 KB heap while awake) and `keepAlive` from the loop; the REST
   TTS path becomes the only voice. Measure heap before and after.

5. **Stream the Gemini request body** straight to the socket instead of
   building ~25 KB of `String`s (system prompt, history, the 4.7 KB
   `DECLARATIONS` copy) during the TLS handshake.

6. **Serial file upload to the SD card.** A console command that takes path,
   size and checksum, receives base64 lines, writes the file, and verifies it.
   Lets `boot.fs`, tuning scripts and `Diana.bin` be pushed to the card without
   removing it.

7. **Retire or implement the inert settings** `idle_sleep`, `auto_wake`,
   `voice_wake` (stored, shown in the menu and tools, do nothing). Decide per
   setting; the skills audit should settle it.

8. **Filtered JSON parsing** of Gemini replies (`DeserializationOption::Filter`)
   so thought signatures and grounding metadata never land in RAM.

9. **TLS verification by default.** Embed the Google Trust Services roots, keep
   `/diana/ca.pem` as the override. Needs the device on the bench when it lands:
   a wrong chain takes Diana offline.

10. **Host tests** for the base64 streamer and the chunked HTTP reader in a
   native PlatformIO environment.

## Hardware add-ons

- **External 2.8" ILI9341 240×320 SPI TFT with XPT2046 touch** (owner has
  several). Preferred over the Nokia 5110 PCD8544 module, which at 84×48 is
  smaller than the built-in 240×135 screen.
  - Bus: share the microSD SPI bus (SCK 40, MOSI 14, MISO 39); needs three
    extra GPIOs: LCD CS, LCD D/C, touch CS (touch IRQ optional; RST can tie to
    3V3, backlight to 3V3 or a PWM pin). The Grove port gives two (G1/G2, used
    as I2C today). **Verify the Cardputer ADV expansion-header pinout** against
    M5's docs before buying into a wiring plan; without a third pin, drop touch.
  - RAM: a 240×320×16-bit framebuffer is 150 KB and will not fit beside TLS on
    this no-PSRAM board. Draw direct or in strips; M5GFX/LovyanGFX drive the
    ILI9341 natively via a custom panel config.
  - UI: the HUD canvases are sized for 240×135; a second layout (portrait log,
    touch buttons for talk/menu) is its own design task.
  - Power: backlight adds roughly 40–80 mA on battery; the module's regulator
    accepts 3.3 V logic.

## Open questions

- **Pop before TTS:** fix in `7f80734` (`codec_hold`), awaiting a listening
  verdict. If a click remains, the next suspect is the I2S clock restart on
  every mic/speaker switch.

## Housekeeping

- Delete the superseded remote branches `pio-migration` and
  `pio-migration-base` (`develop` carries everything).
- Promote the linker-map size analyzer (Pop-11, `mapsize.p`) into
  `~/pop11-tools/`; it works on any ESP32 PlatformIO map.

## Done

- PlatformIO pin, repo cleanup, packager fix — `develop`
- Quality sweep (correctness, security, dead code, `main.cpp` split) — `develop`
- ESPIDFORTH embed, tunables, `boot.fs`; upstream limits PR merged — `develop`
- README build/install guide; CI removed — `develop`
