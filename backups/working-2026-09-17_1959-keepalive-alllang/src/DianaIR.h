// IR remote control via the Cardputer ADV's IR LED (GPIO44), using IRremoteESP8266.
// Send-only: the board has an emitter but no receiver, so Diana transmits known codes.
#pragma once
#include <Arduino.h>

// Basic remotes. protocol: "nec" (default, builds a code from address+command),
// "necraw"/"samsung"/"sony"/"rc5" (command carries the full code).
String dianaIrSend(const String& protocol, uint32_t address, uint32_t command);

// Air conditioner. brand e.g. coolix, daikin, mitsubishi, gree, samsung, lg, panasonic,
// fujitsu, hitachi, toshiba. mode: cool|heat|auto|dry|fan. fan: auto|low|medium|high.
String dianaAc(const String& brand, bool power, const String& mode, int tempC, const String& fan);

// Named IR macros on SD (/diana/ir_codes.json) — teach a device's code once, replay by name.
// Works for projectors, TVs, fans, anything IR. Names are matched case-insensitively.
String dianaIrSaveNamed(const String& name, const String& protocol, uint32_t address, uint32_t command);
String dianaIrRunNamed(const String& name);
String dianaIrList();
