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

2. **Make Gemini Live a build option** (`-DDIANA_LIVE=0`). Drops the second TLS
   session (~70–90 KB heap while awake) and `keepAlive` from the loop; the REST
   TTS path becomes the only voice. Measure heap before and after.

3. **Stream the Gemini request body** straight to the socket instead of
   building ~25 KB of `String`s (system prompt, history, the 4.7 KB
   `DECLARATIONS` copy) during the TLS handshake.

4. **Serial file upload to the SD card.** A console command that takes path,
   size and checksum, receives base64 lines, writes the file, and verifies it.
   Lets `boot.fs`, tuning scripts and `Diana.bin` be pushed to the card without
   removing it.

5. **Retire or implement the inert settings** `idle_sleep`, `auto_wake`,
   `voice_wake` (stored, shown in the menu and tools, do nothing). Decide per
   setting; the skills audit should settle it.

6. **Filtered JSON parsing** of Gemini replies (`DeserializationOption::Filter`)
   so thought signatures and grounding metadata never land in RAM.

7. **TLS verification by default.** Embed the Google Trust Services roots, keep
   `/diana/ca.pem` as the override. Needs the device on the bench when it lands:
   a wrong chain takes Diana offline.

8. **Host tests** for the base64 streamer and the chunked HTTP reader in a
   native PlatformIO environment.

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
