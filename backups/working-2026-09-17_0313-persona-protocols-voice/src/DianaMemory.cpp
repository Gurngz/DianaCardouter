#include "DianaMemory.h"
#include "config.h"
#include <Preferences.h>
#include <SD.h>
#include <time.h>

DianaMemory Memory;

static const char* CATEGORIES[] = {"identity", "preferences", "projects", "relationships", "wishes", "notes"};

bool DianaMemory::validCategory(const String& c) {
    for (auto cat : CATEGORIES) if (c == cat) return true;
    return false;
}

static String todayStr() {
    time_t now = time(nullptr);
    if (now < 1600000000) return "unknown";
    struct tm t;
    localtime_r(&now, &t);
    char buf[16];
    strftime(buf, sizeof(buf), "%Y-%m-%d", &t);
    return String(buf);
}

void DianaMemory::begin(bool sdAvailable) {
    _sd = sdAvailable;
    if (!load()) {
        _doc.clear();
    }
    for (auto cat : CATEGORIES) {
        if (!_doc[cat].is<JsonObject>()) _doc[cat].to<JsonObject>();
    }
}

bool DianaMemory::load() {
    _doc.clear();
    if (_sd) {
        File f = SD.open(MEMORY_PATH, FILE_READ);
        if (f) {
            DeserializationError e = deserializeJson(_doc, f);
            f.close();
            if (!e && _doc.is<JsonObject>()) return true;
            Serial.printf("[MEM] memory.json error: %s\n", e.c_str());
        }
    }
    Preferences p;
    if (p.begin("dianamem", true)) {
        String s = p.getString("json", "");
        p.end();
        if (s.length()) {
            DeserializationError e = deserializeJson(_doc, s);
            if (!e && _doc.is<JsonObject>()) return true;
        }
    }
    _doc.clear();
    return false;
}

bool DianaMemory::save() {
    trimToLimit();
    String s;
    serializeJson(_doc, s);
    bool ok = false;
    if (_sd) {
        if (!SD.exists(DIANA_DIR)) SD.mkdir(DIANA_DIR);
        File f = SD.open(MEMORY_PATH, FILE_WRITE);
        if (f) {
            serializeJsonPretty(_doc, f);
            f.close();
            ok = true;
        }
    }
    Preferences p;
    if (p.begin("dianamem", false)) {
        if (s.length() < 3900) p.putString("json", s);
        p.end();
        ok = true;
    }
    return ok;
}

void DianaMemory::trimToLimit() {
    // Drop the oldest entries (by "updated") until under MEMORY_MAX_CHARS, like the PC build.
    for (int guard = 0; guard < 64; ++guard) {
        if (measureJson(_doc) <= MEMORY_MAX_CHARS) return;
        String oldestCat, oldestKey, oldestDate = "9999-99-99";
        for (auto cat : CATEGORIES) {
            for (JsonPair kv : _doc[cat].as<JsonObject>()) {
                String d = kv.value()["updated"] | "0000-00-00";
                if (d < oldestDate) { oldestDate = d; oldestCat = cat; oldestKey = kv.key().c_str(); }
            }
        }
        if (oldestKey.isEmpty()) return;
        _doc[oldestCat].as<JsonObject>().remove(oldestKey);
        Serial.printf("[MEM] trimmed %s/%s\n", oldestCat.c_str(), oldestKey.c_str());
    }
}

String DianaMemory::remember(const String& categoryIn, const String& keyIn, const String& valueIn) {
    String category = categoryIn;
    category.toLowerCase();
    if (!validCategory(category)) category = "notes";
    String key = keyIn;
    key.trim();
    key.replace(' ', '_');
    key.toLowerCase();
    if (key.isEmpty()) return "Error: empty key";
    String value = valueIn;
    value.trim();
    if (value.length() > MEMORY_VALUE_MAX) value = value.substring(0, MEMORY_VALUE_MAX) + "...";
    JsonObject entry = _doc[category][key].to<JsonObject>();
    entry["value"] = value;
    entry["updated"] = todayStr();
    save();
    return "Remembered: " + category + "/" + key + " = " + value;
}

String DianaMemory::forget(const String& categoryIn, const String& key) {
    String category = categoryIn;
    category.toLowerCase();
    if (!validCategory(category)) category = "notes";
    JsonObject cat = _doc[category].as<JsonObject>();
    if (cat[key].isNull()) return "Not found: " + category + "/" + key;
    cat.remove(key);
    save();
    return "Forgotten: " + category + "/" + key;
}

int DianaMemory::count() {
    int n = 0;
    for (auto cat : CATEGORIES) n += _doc[cat].as<JsonObject>().size();
    return n;
}

String DianaMemory::userName() {
    const char* v = _doc["identity"]["name"]["value"] | "";
    return String(v);
}

static String titleCase(const char* key) {
    String s(key);
    s.replace('_', ' ');
    bool up = true;
    for (size_t i = 0; i < s.length(); ++i) {
        if (up && s[i] >= 'a' && s[i] <= 'z') s[i] = s[i] - 32;
        up = (s[i] == ' ');
    }
    return s;
}

String DianaMemory::formatForPrompt() {
    if (count() == 0) return "";
    String out = "[WHAT YOU KNOW ABOUT THIS PERSON - use naturally, never recite like a list]\n";
    struct { const char* cat; const char* title; int limit; } sections[] = {
        {"identity", nullptr, 20}, {"preferences", "Preferences:", 15}, {"projects", "Active Projects / Goals:", 8},
        {"relationships", "People in their life:", 10}, {"wishes", "Wishes / Plans / Wants:", 8}, {"notes", "Other notes:", 8}};
    for (auto& sec : sections) {
        JsonObject obj = _doc[sec.cat].as<JsonObject>();
        if (obj.size() == 0) continue;
        if (sec.title) { out += "\n"; out += sec.title; out += "\n"; }
        int n = 0;
        for (JsonPair kv : obj) {
            if (n++ >= sec.limit) break;
            const char* v = kv.value()["value"] | "";
            if (!*v) continue;
            if (sec.title) out += "  - ";
            out += titleCase(kv.key().c_str());
            out += ": ";
            out += v;
            out += "\n";
        }
    }
    if (out.length() > 2000) out = out.substring(0, 1997) + "...";
    return out;
}

String DianaMemory::dump() {
    String out;
    for (auto cat : CATEGORIES) {
        JsonObject obj = _doc[cat].as<JsonObject>();
        for (JsonPair kv : obj) {
            out += String(cat) + "/" + kv.key().c_str() + " = " + (kv.value()["value"] | "") + "\n";
        }
    }
    if (out.isEmpty()) out = "(memory is empty)\n";
    return out;
}
