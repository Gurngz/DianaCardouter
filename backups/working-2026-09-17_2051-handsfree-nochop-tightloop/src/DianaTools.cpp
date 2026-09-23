#include "DianaTools.h"
#include "DianaMemory.h"
#include "DianaNet.h"
#include "config.h"
#include "DianaIR.h"
#include "DianaMusic.h"
#include <SD.h>
#include <vector>

namespace DianaTools {

// Terse descriptions on purpose: this whole blob is resent on every chat call, so every
// word here is input tokens per turn. Keep names/params; trim prose.
const char DECLARATIONS[] PROGMEM = R"JSON([{"functionDeclarations":[
{"name":"save_memory","description":"Remember a fact about the user long-term.","parameters":{"type":"OBJECT","properties":{"category":{"type":"STRING","description":"identity|preferences|projects|relationships|wishes|notes"},"key":{"type":"STRING","description":"snake_case"},"value":{"type":"STRING"}},"required":["category","key","value"]}},
{"name":"forget_memory","description":"Delete a remembered fact.","parameters":{"type":"OBJECT","properties":{"category":{"type":"STRING"},"key":{"type":"STRING"}},"required":["category","key"]}},
{"name":"weather_report","description":"Weather for a city, or 'here' for current location.","parameters":{"type":"OBJECT","properties":{"city":{"type":"STRING"}},"required":["city"]}},
{"name":"set_timer","description":"Start a countdown timer that beeps when done.","parameters":{"type":"OBJECT","properties":{"seconds":{"type":"INTEGER"},"label":{"type":"STRING"}},"required":["seconds"]}},
{"name":"take_note","description":"Append a note to the SD card.","parameters":{"type":"OBJECT","properties":{"text":{"type":"STRING"}},"required":["text"]}},
{"name":"read_notes","description":"Read recent notes.","parameters":{"type":"OBJECT","properties":{"count":{"type":"INTEGER"}}}},
{"name":"get_device_status","description":"Battery, WiFi, SD, memory, uptime, clock.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"device_settings","description":"Set volume/brightness (0-255) or voice/auto_wake/boot_music/hands_free (on/off).","parameters":{"type":"OBJECT","properties":{"setting":{"type":"STRING"},"value":{"type":"STRING"}},"required":["setting","value"]}},
{"name":"web_search","description":"Search the web for current facts.","parameters":{"type":"OBJECT","properties":{"query":{"type":"STRING"}},"required":["query"]}},
{"name":"connect_wifi","description":"Join a WiFi network (saves it and reconnects).","parameters":{"type":"OBJECT","properties":{"ssid":{"type":"STRING"},"password":{"type":"STRING"}},"required":["ssid"]}},
{"name":"scan_wifi","description":"List nearby WiFi networks with signal and security.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"where_am_i","description":"Approximate location (city) from the internet connection.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"open_settings","description":"Open the on-screen settings menu.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"ir_send","description":"Transmit an infrared remote code from the IR LED. protocol nec|samsung|sony|rc5|necraw.","parameters":{"type":"OBJECT","properties":{"protocol":{"type":"STRING"},"address":{"type":"INTEGER"},"command":{"type":"INTEGER"}},"required":["command"]}},
{"name":"ac_control","description":"Control an air conditioner over IR. brand: coolix|daikin|mitsubishi|gree|samsung|lg|panasonic|fujitsu|hitachi|toshiba. mode: cool|heat|auto|dry|fan. fan: auto|low|medium|high.","parameters":{"type":"OBJECT","properties":{"brand":{"type":"STRING"},"power":{"type":"BOOLEAN"},"mode":{"type":"STRING"},"temperature":{"type":"INTEGER"},"fan":{"type":"STRING"}},"required":["brand"]}},
{"name":"ir_save","description":"Save a named IR command (e.g. 'projector power', 'tv volume up') to replay later by name. Ask the user for the code if unknown.","parameters":{"type":"OBJECT","properties":{"name":{"type":"STRING"},"protocol":{"type":"STRING"},"address":{"type":"INTEGER"},"command":{"type":"INTEGER"}},"required":["name","command"]}},
{"name":"ir_run","description":"Send a previously saved named IR command, e.g. 'projector power'.","parameters":{"type":"OBJECT","properties":{"name":{"type":"STRING"}},"required":["name"]}},
{"name":"ir_list","description":"List the saved named IR commands.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"music_play","description":"Play built-in pixel/chiptune music with an on-screen visualizer. Optional track name (e.g. Arcade, Starlight, Blocks, Neon Run, Ode to Joy).","parameters":{"type":"OBJECT","properties":{"track":{"type":"STRING"}}}},
{"name":"music_stop","description":"Stop the music.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"music_next","description":"Skip to the next music track.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"music_list","description":"List the built-in music tracks.","parameters":{"type":"OBJECT","properties":{}}},
{"name":"mute_diana","description":"Mute/unmute voice output.","parameters":{"type":"OBJECT","properties":{"muted":{"type":"BOOLEAN"}},"required":["muted"]}},
{"name":"shutdown_diana","description":"Power the device off (Barnyard Protocol).","parameters":{"type":"OBJECT","properties":{}}}
]}])JSON";

bool isNetworkTool(const String& name) {
    return name == "weather_report" || name == "web_search" ||
           name == "connect_wifi" || name == "scan_wifi" || name == "where_am_i";
}

static String jsonResult(const String& text) {
    JsonDocument d;
    d["result"] = text;
    String s;
    serializeJson(d, s);
    return s;
}

static String takeNote(const String& text) {
    if (!SD.cardSize()) return "Error: no SD card, cannot save notes.";
    if (!SD.exists(DIANA_DIR)) SD.mkdir(DIANA_DIR);
    File f = SD.open(NOTES_PATH, FILE_APPEND);
    if (!f) return "Error: cannot open notes file.";
    String stamp = Net.timeString("%Y-%m-%d %H:%M");
    if (stamp.isEmpty()) stamp = "(no clock)";
    f.print(stamp);
    f.print(" | ");
    f.println(text);
    f.close();
    return "Note saved: " + text;
}

static String readNotes(int count) {
    if (count <= 0) count = 8;
    if (count > 20) count = 20;
    File f = SD.open(NOTES_PATH, FILE_READ);
    if (!f) return "No notes yet.";
    // read the tail of the file (max 3 KB) and keep the last `count` lines
    size_t size = f.size();
    size_t start = size > 3000 ? size - 3000 : 0;
    f.seek(start);
    String tail;
    while (f.available()) tail += (char)f.read();
    f.close();
    if (start > 0) { int nl = tail.indexOf('\n'); if (nl >= 0) tail = tail.substring(nl + 1); }
    // split lines
    std::vector<String> lines;
    int s = 0;
    while (s < (int)tail.length()) {
        int nl = tail.indexOf('\n', s);
        String l = (nl < 0) ? tail.substring(s) : tail.substring(s, nl);
        l.trim();
        if (l.length()) lines.push_back(l);
        if (nl < 0) break;
        s = nl + 1;
    }
    if (lines.empty()) return "No notes yet.";
    String out;
    int from = (int)lines.size() > count ? (int)lines.size() - count : 0;
    for (int i = from; i < (int)lines.size(); ++i) { out += lines[i]; out += "\n"; }
    return out;
}

String execute(const String& name, JsonObjectConst args, ToolHooks& hooks) {
    Serial.printf("[TOOL] %s\n", name.c_str());
    if (name == "save_memory") {
        return jsonResult(Memory.remember(args["category"] | "notes", args["key"] | "", args["value"] | ""));
    }
    if (name == "forget_memory") {
        return jsonResult(Memory.forget(args["category"] | "notes", args["key"] | ""));
    }
    if (name == "weather_report") {
        WeatherInfo w;
        String city = args["city"] | "here";
        if (!Net.isConnected()) return jsonResult("Error: no WiFi connection.");
        if (!Net.weatherFor(city, w)) return jsonResult("Error: could not fetch weather for " + city);
        return jsonResult("Weather in " + w.city + ": " + w.summary());
    }
    if (name == "set_timer") {
        int secs = args["seconds"] | 0;
        String label = args["label"] | "timer";
        if (secs <= 0) return jsonResult("Error: seconds must be positive.");
        if (!hooks.addTimer) return jsonResult("Error: timers unavailable.");
        return jsonResult(hooks.addTimer(secs, label));
    }
    if (name == "take_note") {
        return jsonResult(takeNote(args["text"] | ""));
    }
    if (name == "read_notes") {
        return jsonResult(readNotes(args["count"] | 8));
    }
    if (name == "get_device_status") {
        return jsonResult(hooks.deviceStatus ? hooks.deviceStatus() : String("unavailable"));
    }
    if (name == "device_settings") {
        if (!hooks.deviceSetting) return jsonResult("Error: unavailable.");
        return jsonResult(hooks.deviceSetting(args["setting"] | "", args["value"] | ""));
    }
    if (name == "web_search") {
        if (!hooks.webSearch) return jsonResult("Error: search unavailable.");
        return jsonResult(hooks.webSearch(args["query"] | ""));
    }
    if (name == "connect_wifi") {
        if (!hooks.connectWifi) return jsonResult("Error: unavailable.");
        return jsonResult(hooks.connectWifi(args["ssid"] | "", args["password"] | ""));
    }
    if (name == "scan_wifi") {
        return jsonResult(hooks.scanWifi ? hooks.scanWifi() : String("unavailable"));
    }
    if (name == "where_am_i") {
        return jsonResult(hooks.whereAmI ? hooks.whereAmI() : String("unavailable"));
    }
    if (name == "open_settings") {
        if (hooks.openSettings) hooks.openSettings();
        return jsonResult("Opening the settings menu on screen.");
    }
    if (name == "ir_send") {
        uint32_t addr = (uint32_t)(args["address"] | 0);
        uint32_t cmd = (uint32_t)(args["command"] | 0);
        return jsonResult(dianaIrSend(args["protocol"] | "nec", addr, cmd));
    }
    if (name == "ac_control") {
        return jsonResult(dianaAc(args["brand"] | "", args["power"] | true,
                                  args["mode"] | "cool", args["temperature"] | 24, args["fan"] | "auto"));
    }
    if (name == "ir_save") {
        uint32_t addr = (uint32_t)(args["address"] | 0);
        uint32_t cmd = (uint32_t)(args["command"] | 0);
        return jsonResult(dianaIrSaveNamed(args["name"] | "", args["protocol"] | "nec", addr, cmd));
    }
    if (name == "ir_run") {
        return jsonResult(dianaIrRunNamed(args["name"] | ""));
    }
    if (name == "ir_list") {
        return jsonResult(dianaIrList());
    }
    if (name == "music_play") {
        return jsonResult(hooks.musicPlay ? hooks.musicPlay(args["track"] | "") : String("music unavailable"));
    }
    if (name == "music_stop") { return jsonResult(Music.stop()); }
    if (name == "music_next") { return jsonResult(Music.isOpen() ? Music.next() : (hooks.musicPlay ? hooks.musicPlay("") : String("music unavailable"))); }
    if (name == "music_list") { return jsonResult("Tracks: " + Music.list()); }
    if (name == "mute_diana") {
        bool m = args["muted"] | true;
        if (hooks.setMuted) hooks.setMuted(m);
        return jsonResult(m ? "Voice muted. Text replies continue." : "Voice unmuted.");
    }
    if (name == "shutdown_diana") {
        if (hooks.requestShutdown) hooks.requestShutdown();
        return jsonResult("Shutdown armed. Say your farewell now; the device powers off after you finish speaking.");
    }
    return jsonResult("Error: unknown tool " + name);
}

}  // namespace DianaTools
