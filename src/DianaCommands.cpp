// Slash commands typed on the HUD, in standby, or over the serial console.
#include <ArduinoJson.h>
#include <SD.h>
#include "config.h"
#include "DianaApp.h"
#include "DianaConfig.h"
#include "DianaMemory.h"
#include "DianaUI.h"
#include "DianaAudio.h"
#include "DianaNet.h"
#include "DianaGemini.h"
#include "DianaTools.h"
#include "DianaMenu.h"
#include "DianaIR.h"

// "<proto> <addr> <cmd>" -> parts; missing proto defaults to nec, numbers hex if 0x-prefixed.
static void parseIrArgs(const String& argIn, String& proto, uint32_t& addr, uint32_t& cmd) {
    String arg = argIn; arg.trim();
    int s1 = arg.indexOf(' ');
    proto = s1 < 0 ? arg : arg.substring(0, s1);
    String rest = s1 < 0 ? "" : arg.substring(s1 + 1); rest.trim();
    int s2 = rest.indexOf(' ');
    addr = (uint32_t)strtoul((s2 < 0 ? String("0") : rest.substring(0, s2)).c_str(), nullptr, 0);
    cmd  = (uint32_t)strtoul((s2 < 0 ? rest : rest.substring(s2 + 1)).c_str(), nullptr, 0);
    if (proto.isEmpty()) proto = "nec";
}

// Model ids are spliced into the request path: allow only the characters Google uses.
static bool isModelId(const String& s) {
    if (s.isEmpty()) return true;                 // empty = default
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        if (!(isalnum((unsigned char)c) || c == '-' || c == '.' || c == ':' || c == '_')) return false;
    }
    return s.length() <= 64;
}

// ── slash commands ───────────────────────────────────────────────────────────
static void showHelp() {
    UI.log("say 'diana ...' hands-free | TAB talk | /settings menu", 's');
    UI.log("keys: ENTER send | Fn+;/. scroll | Fn+` esc", 's');
    UI.log("/setup /wifi <ssid> <pass> /key <apikey> /name <n>", 's');
    UI.log("/voice on|off|<Voice> /mute /unmute /vol N /bright N", 's');
    UI.log("/handsfree on|off /wake <word> /mic <8-128> /vad <n> /beep", 's');
    UI.log("/music [track] /ir /ac /irsave /irrun /irlist", 's');
    UI.log("/memory /forget cat/key /notes /clear /status", 's');
    UI.log("/model <id> /ttsmodel <id> /think minimal|low|", 's');
    UI.log("/sleep /reboot /off", 's');
}

bool handleCommand(const String& lineIn) {
    String line = lineIn;
    line.trim();
    if (!line.startsWith("/")) return false;
    int sp = line.indexOf(' ');
    String cmd = sp < 0 ? line : line.substring(0, sp);
    String arg = sp < 0 ? "" : line.substring(sp + 1);
    arg.trim();
    cmd.toLowerCase();
    Audio.chirpAck();

    if (cmd == "/help" || cmd == "/?") { showHelp(); }
    else if (cmd == "/settings" || cmd == "/menu") { Menu.open(); }
    else if (cmd == "/music") { musicReq = true; musicTrack = arg; }
    else if (cmd == "/setup") { openSetupPortal(); }
    else if (cmd == "/wifi") {
        int s2 = arg.indexOf(' ');
        String ssid = s2 < 0 ? arg : arg.substring(0, s2);
        String pass = s2 < 0 ? "" : arg.substring(s2 + 1);
        if (ssid.isEmpty()) { UI.log("usage: /wifi <ssid> <password>", 'w'); return true; }
        Config.setWifi(ssid, pass);
        Config.save(sdOk);
        UI.log("wifi saved. connecting...", 's');
        Gemini.disconnect();
        if (Net.connect(10000, nullptr)) { UI.log("connected: " + Net.ip(), 's'); Net.syncTime(Config.tz); }
        else UI.log("could not connect", 'e');
    }
    else if (cmd == "/key" || cmd == "/addkey") { if (arg.length() < 12) { UI.log("usage: /key <gemini api key>", 'w'); return true; } Config.addApiKey(arg); Config.syncActiveKey(); Config.save(sdOk); UI.log("api key added (" + String(Config.apiKeyCount()) + " total)", 's'); }
    else if (cmd == "/keys") { UI.log(String("api keys: ") + Config.apiKeyCount() + ", active #" + Config.apiKeyIndex, 's'); }
    else if (cmd == "/nextkey") { UI.log(Config.rotateApiKey() ? "switched to key #" + String(Config.apiKeyIndex) : "only one key", 's'); }
    else if (cmd == "/delkey") { if (arg.isEmpty() || !isDigit(arg[0])) { UI.log("usage: /delkey <index> (see /keys)", 'w'); return true; } int n = arg.toInt(); if (Config.removeApiKey(n)) { Config.save(sdOk); UI.log("removed key #" + String(n) + " (" + Config.apiKeyCount() + " left)", 's'); } else UI.log("no key #" + String(n) + " (have " + Config.apiKeyCount() + ")", 'w'); }
    else if (cmd == "/font") { String a = arg; a.toLowerCase(); Config.font = (a == "ascii") ? "ascii" : "jp"; UI.setFont(Config.font != "ascii"); Config.save(sdOk); UI.log(Config.font == "ascii" ? "font: plain ASCII" : "font: multilingual (EN/FR/ES/JP)", 's'); }
    else if (cmd == "/name") { Config.userName = arg; Config.save(sdOk); UI.log("name: " + arg, 's'); }
    else if (cmd == "/voice") {
        String a = arg; a.toLowerCase();
        if (a == "on" || a == "off") { UI.log(applyDeviceSetting("voice", a), 's'); }
        else if (arg.length()) { Config.ttsVoice = arg; Config.save(sdOk); UI.log("tts voice: " + arg, 's'); }
        else UI.log(String("voice is ") + (Config.voiceEnabled ? "on" : "off") + ", " + Config.ttsVoiceOrDefault(), 's');
    }
    else if (cmd == "/mute") { muted = true; refreshStatusBar(); UI.log("muted", 's'); }
    else if (cmd == "/unmute") { muted = false; refreshStatusBar(); UI.log("unmuted", 's'); }
    else if (cmd == "/vol") { UI.log(applyDeviceSetting("volume", arg), 's'); }
    else if (cmd == "/bright") { UI.log(applyDeviceSetting("brightness", arg), 's'); }
    else if (cmd == "/handsfree") { UI.log(applyDeviceSetting("hands_free", arg.length() ? arg : String("on")), 's'); }
    else if (cmd == "/voicewake") { UI.log(applyDeviceSetting("voice_wake", arg.length() ? arg : String("on")), 's'); }
    else if (cmd == "/mic") { Config.micGain = constrain(arg.toInt(), 8, 128); Config.save(sdOk); UI.log("mic gain " + String(Config.micGain) + " (reboot to apply)", 's'); }
    else if (cmd == "/vad") { Config.vadThreshold = constrain(arg.toInt(), 100, 4000); Config.save(sdOk); UI.log("voice threshold " + String(Config.vadThreshold) + " (lower = hears more)", 's'); }
    else if (cmd == "/idle" || cmd == "/sleeptimeout") { UI.log(applyDeviceSetting("sleep_timeout", arg), 's'); }
    else if (cmd == "/wake") { Config.wakeWord = arg; Config.save(sdOk); UI.log("wake word: " + (arg.length() ? arg : String("(none)")), 's'); }
    else if (cmd == "/beep") { Audio.bootTest(); UI.log("test tone", 's'); }
    else if (cmd == "/ir") {
        // /ir <proto> <addr> <cmd>  (numbers hex if 0x-prefixed, else decimal)
        String proto; uint32_t a, c;
        parseIrArgs(arg, proto, a, c);
        UI.log(dianaIrSend(proto, a, c), 's');
    }
    else if (cmd == "/irsave") {
        // /irsave <name>|<proto> <addr> <cmd>   e.g. /irsave projector power|nec 0 0x1234
        int bar = arg.indexOf('|');
        String nm = bar < 0 ? arg : arg.substring(0, bar);
        String proto; uint32_t ad, cc;
        parseIrArgs(bar < 0 ? String("") : arg.substring(bar + 1), proto, ad, cc);
        UI.log(dianaIrSaveNamed(nm, proto, ad, cc), 's');
    }
    else if (cmd == "/irrun") { UI.log(dianaIrRunNamed(arg), 's'); }
    else if (cmd == "/irlist") { UI.log(dianaIrList(), 's'); }
    else if (cmd == "/ac") {
        // /ac <brand> <on|off> <mode> <temp> <fan>
        String t = arg; t.trim();
        int i1 = t.indexOf(' '); String brand = i1 < 0 ? t : t.substring(0, i1); t = i1 < 0 ? "" : t.substring(i1 + 1);
        int i2 = t.indexOf(' '); String pw = i2 < 0 ? t : t.substring(0, i2); t = i2 < 0 ? "" : t.substring(i2 + 1);
        int i3 = t.indexOf(' '); String md = i3 < 0 ? t : t.substring(0, i3); t = i3 < 0 ? "" : t.substring(i3 + 1);
        int i4 = t.indexOf(' '); String tp = i4 < 0 ? t : t.substring(0, i4); String fn = i4 < 0 ? "auto" : t.substring(i4 + 1);
        if (brand.isEmpty()) { UI.log("usage: /ac <brand> <on|off> <mode> <temp> <fan>", 'w'); return true; }
        bool on = !(pw == "off" || pw == "0");
        UI.log(dianaAc(brand, on, md.length() ? md : String("cool"), tp.toInt() ? tp.toInt() : 24, fn), 's');
    }
    else if (cmd == "/memory") { String d = Memory.dump(); UI.log(d, 's'); }
    else if (cmd == "/forget") {
        int sl = arg.indexOf('/');
        if (sl < 0) { UI.log("usage: /forget category/key", 'w'); return true; }
        UI.log(Memory.forget(arg.substring(0, sl), arg.substring(sl + 1)), 's');
    }
    else if (cmd == "/notes") { JsonDocument d; ToolHooks h; UI.log(DianaTools::execute("read_notes", d.as<JsonObjectConst>(), h), 's'); }
    else if (cmd == "/clear") { Gemini.clearHistory(); if (sdOk) SD.remove(HISTORY_PATH); UI.log("conversation cleared (incl. saved history)", 's'); }
    else if (cmd == "/status") { UI.log(deviceStatusText(), 's'); }
    else if (cmd == "/model") {
        if (!isModelId(arg)) { UI.log("usage: /model <id>  (letters, digits, - . :)", 'w'); return true; }   // goes into a URL path
        Config.chatModel = arg; Config.save(sdOk); UI.log(String("chat model: ") + Config.chatModelOrDefault(), 's');
    }
    else if (cmd == "/think") { Config.thinkingLevel = arg; Config.save(sdOk); UI.log("thinking level: " + (arg.length() ? arg : String("(model default)")), 's'); }
    else if (cmd == "/ttsmodel") {
        if (!isModelId(arg)) { UI.log("usage: /ttsmodel <id>", 'w'); return true; }
        Config.ttsModel = arg; Config.save(sdOk); UI.log(String("tts model: ") + Config.ttsModelOrDefault(), 's');
    }
    else if (cmd == "/sleep") { enterStandby(); }
    else if (cmd == "/reboot") { ESP.restart(); }
    else if (cmd == "/off") { shutdownArmed = true; runTurn("Barnyard Protocol.", nullptr, 0); }
    else if (cmd == "/heap") { UI.log("free heap " + String(ESP.getFreeHeap()) + " largest " + String(ESP.getMaxAllocHeap()), 's'); }
    else { UI.log("unknown command. /help", 'w'); }
    return true;
}

