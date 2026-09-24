# espidforth (vendored)

`forth_core.cpp/.h`, `forth_version.h` and `LICENSE` are an **unmodified copy** of
<https://github.com/IoTone/ESPIDFORTH> `components/forth` at `8651fcc`
(v0.5.0 + PR #1 "configurable limits", merged 2026-09-24).

`library.json` sizes the engine for this no-PSRAM board (160 words, 24-char
names, 1536 code cells: about 14 KB of static RAM instead of 57 KB) and adds
the three `-D` shims that ESP-IDF 4.4 (Arduino core 2.0.17) needs:
`EXT_RAM_BSS_ATTR=` (empty without PSRAM), `CHIP_ESP32C6=13`, and the consumer
must `#undef MAX_INPUT` before including `forth_core.h` (Arduino defines it).

To update: copy the four upstream files over these and note the commit here.
