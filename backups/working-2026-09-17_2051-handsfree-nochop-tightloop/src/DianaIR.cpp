#include <Arduino.h>
#include <IRremoteESP8266.h>
#include <IRsend.h>
#include <IRac.h>
#include <IRutils.h>
#include <SD.h>
#include <ArduinoJson.h>
#include "DianaIR.h"
#include "config.h"

static const uint16_t IR_LED = 44;      // Cardputer ADV IR emitter
static IRsend irsend(IR_LED);
static IRac   ac(IR_LED);
static bool   s_begun = false;

static void ensureBegun() {
    if (!s_begun) { irsend.begin(); s_begun = true; }
}

String dianaIrSend(const String& protocolIn, uint32_t address, uint32_t command) {
    ensureBegun();
    String p = protocolIn;
    p.toLowerCase();
    p.trim();
    if (p == "samsung") {
        irsend.sendSAMSUNG((uint64_t)command, 32);
    } else if (p == "sony") {
        irsend.sendSony((uint64_t)command, 12);
    } else if (p == "rc5") {
        irsend.sendRC5((uint64_t)command, 13);
    } else if (p == "necraw" || p == "raw" || p == "hex") {
        irsend.sendNEC((uint64_t)command, 32);
    } else {
        p = "nec";
        uint64_t code;
        if (command > 0xFFFF) {
            code = command;                       // already a full code
        } else {                                  // build a standard 32-bit NEC frame
            uint32_t a = address & 0xFF, c = command & 0xFF;
            code = ((uint32_t)a << 24) | (((~a) & 0xFF) << 16) | (c << 8) | ((~c) & 0xFF);
        }
        irsend.sendNEC(code, 32);
    }
    char buf[80];
    snprintf(buf, sizeof(buf), "Sent IR (%s) cmd 0x%lX.", p.c_str(), (unsigned long)command);
    return String(buf);
}

String dianaAc(const String& brand, bool power, const String& modeIn, int tempC, const String& fanIn) {
    ensureBegun();
    decode_type_t proto = strToDecodeType(brand.c_str());
    if (proto == decode_type_t::UNKNOWN || !IRac::isProtocolSupported(proto)) {
        return "AC brand not supported: " + brand +
               ". Try coolix, daikin, mitsubishi, gree, samsung, lg, panasonic, fujitsu, hitachi or toshiba.";
    }
    if (tempC < 16) tempC = 24;
    if (tempC > 30) tempC = 30;
    ac.next.protocol = proto;
    ac.next.model    = -1;
    ac.next.power    = power;
    ac.next.mode     = IRac::strToOpmode(modeIn.length() ? modeIn.c_str() : "cool");
    ac.next.celsius  = true;
    ac.next.degrees  = tempC;
    ac.next.fanspeed = IRac::strToFanspeed(fanIn.length() ? fanIn.c_str() : "auto");
    ac.next.swingv   = stdAc::swingv_t::kOff;
    ac.next.swingh   = stdAc::swingh_t::kOff;
    ac.next.light = false; ac.next.beep = false; ac.next.econo = false;
    ac.next.turbo = false; ac.next.quiet = false; ac.next.filter = false; ac.next.clean = false;
    ac.next.sleep = -1; ac.next.clock = -1;
    bool ok = ac.sendAc();
    if (!ok) return "AC send failed (protocol/RMT).";
    return "Sent " + brand + " AC: " + (power ? "on, " : "off, ") + String(tempC) + "C " +
           (modeIn.length() ? modeIn : String("cool")) + ", fan " + (fanIn.length() ? fanIn : String("auto")) + ".";
}

// ── named IR macros (SD: /diana/ir_codes.json) ───────────────────────────
static String irKey(const String& name) { String k = name; k.toLowerCase(); k.trim(); return k; }

String dianaIrSaveNamed(const String& name, const String& protocol, uint32_t address, uint32_t command) {
    String k = irKey(name);
    if (k.isEmpty()) return "Error: the command needs a name.";
    if (!SD.cardSize()) return "Error: no SD card to save IR commands.";
    JsonDocument doc;
    File f = SD.open(IR_CODES_PATH, FILE_READ);
    if (f) { deserializeJson(doc, f); f.close(); }
    JsonObject o = doc[k].to<JsonObject>();
    o["p"] = protocol.length() ? protocol : String("nec");
    o["a"] = address;
    o["c"] = command;
    File w = SD.open(IR_CODES_PATH, FILE_WRITE);
    if (!w) return "Error: couldn't write IR file.";
    serializeJson(doc, w);
    w.close();
    return "Saved IR command '" + k + "'. Say 'run " + k + "' to send it.";
}

String dianaIrRunNamed(const String& name) {
    String k = irKey(name);
    File f = SD.open(IR_CODES_PATH, FILE_READ);
    if (!f) return "No saved IR commands yet.";
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) return "IR file unreadable.";
    if (!doc[k].is<JsonObject>()) return "No IR command named '" + k + "'. Save it first with its code.";
    JsonObject o = doc[k];
    dianaIrSend(o["p"] | "nec", o["a"] | 0, o["c"] | 0);
    return "Sent '" + k + "'.";
}

String dianaIrList() {
    File f = SD.open(IR_CODES_PATH, FILE_READ);
    if (!f) return "No saved IR commands.";
    JsonDocument doc;
    if (deserializeJson(doc, f)) { f.close(); return "IR file unreadable."; }
    f.close();
    String s;
    for (JsonPair kv : doc.as<JsonObject>()) { if (s.length()) s += ", "; s += kv.key().c_str(); }
    return s.length() ? ("Saved IR commands: " + s) : "No saved IR commands.";
}
