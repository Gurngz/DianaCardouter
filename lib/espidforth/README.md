# espidforth (vendored)

`forth_core.cpp/.h`, `forth_version.h` and `LICENSE` come from
<https://github.com/IoTone/ESPIDFORTH> `components/forth` at v0.5.0 (`dec0591`).

Local change: the six `MAX_*` limits are wrapped in `#ifndef` so `library.json`
can size the engine for a no-PSRAM board (160 words, 24-char names, 1536 code
cells: about 14 KB of static RAM instead of 57 KB). Everything else is
untouched; the three `-D` shims in `library.json` cover what ESP-IDF 4.4
(Arduino core 2.0.17) lacks. To update: copy the upstream files over these and
re-apply the guards (or upstream them).
