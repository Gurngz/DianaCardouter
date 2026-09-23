// Long-term memory. Same JSON schema as memory/long_term.json in the PC build:
// { "identity": { "name": {"value": "...", "updated": "YYYY-MM-DD"} }, "preferences": {...}, ... }
// Stored at /diana/memory.json on SD, mirrored to NVS.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

class DianaMemory {
public:
    void   begin(bool sdAvailable);
    String remember(const String& category, const String& key, const String& value);
    String forget(const String& category, const String& key);
    String formatForPrompt();          // "[WHAT YOU KNOW ABOUT THIS PERSON ...]" block
    String dump();                     // compact listing for the /memory command
    int    count();
    bool   isEmpty() { return count() == 0; }
    String userName();                 // identity.name if known

private:
    JsonDocument _doc;
    bool _sd = false;
    bool load();
    bool save();
    void trimToLimit();
    static bool validCategory(const String& c);
};

extern DianaMemory Memory;
