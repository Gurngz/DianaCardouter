// ─────────────────────────────────────────────────────────────────────────────
//  DIANA (D-I-0336-7) — Cardputer ADV edition
//  Pocket body of the PC voice assistant: Gemini chat + voice, welcome protocol,
//  long-term memory on SD, function-calling tools, holographic HUD.
// ─────────────────────────────────────────────────────────────────────────────
#include <M5Cardputer.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <ArduinoJson.h>
#include <esp_sleep.h>
#include <esp_system.h>

#include "config.h"
#include "prompt.h"
#include "DianaApp.h"
#include "DianaConfig.h"
#include "DianaMemory.h"
#include "DianaUI.h"
#include "DianaAudio.h"
#include "DianaNet.h"
#include "DianaGemini.h"
#include "DianaLive.h"
#include "DianaTools.h"
#include "DianaSetup.h"
#include "DianaMenu.h"
#include "DianaIR.h"
#include "DianaMusic.h"
#include "DianaForth.h"

// The TLS handshake, base64 streaming buffers and JSON parsing all run on the
// loop task; the default 8 KB stack is too tight for that chain.
SET_LOOP_TASK_STACK_SIZE(20 * 1024);

// ── app state (shared with DianaCommands.cpp / DianaConsole.cpp via DianaApp.h) ──
bool   sdOk = false;                     // voice replies need SD (audio is streamed through it)
bool   muted = false;
bool   shutdownArmed = false;
bool   awake = false;
String input;
bool   menuOpenReq = false;              // voice tool / command asked to open the settings menu
bool   musicReq = false;                 // voice tool / command asked to open pixel music
String musicTrack;
String g_lastUserMsg;                    // this turn's user message, paired with the reply when persisted
static uint32_t lastStatusMs = 0;

struct DianaTimer { bool active = false; uint32_t endMs = 0; String label; };
static DianaTimer timers[4];

static ToolHooks hooks;

// ── small text helpers ───────────────────────────────────────────────────────
// Cut a UTF-8 string to at most n bytes without splitting a multi-byte sequence.
String utf8Truncate(const String& s, size_t n) {
    if (s.length() <= n) return s;
    size_t i = n;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    return s.substring(0, i);
}
// Remove the last UTF-8 glyph.
void utf8Backspace(String& s) {
    if (s.isEmpty()) return;
    int i = s.length() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xC0) == 0x80) --i;
    s = s.substring(0, i);
}
bool isWakePhrase(const String& lower) {
    return lower.indexOf("wake up diana") >= 0 || lower.indexOf("diana wake up") >= 0 || lower.indexOf("wake up") >= 0;
}

// ── helpers ──────────────────────────────────────────────────────────────────
static void bootLog(const String& s, bool ok = true) { UI.bootLine(s, ok); Serial.println("[BOOT] " + s); }
static void progressCb(const String& s) { bootLog(s); }

static bool mountSd() {
    SPI.begin(SD_PIN_SCK, SD_PIN_MISO, SD_PIN_MOSI, SD_PIN_CS);
    if (!SD.begin(SD_PIN_CS, SPI, 25000000)) {
        if (!SD.begin(SD_PIN_CS, SPI, 10000000)) return false;
    }
    if (SD.cardType() == CARD_NONE) return false;
    if (!SD.exists(DIANA_DIR)) SD.mkdir(DIANA_DIR);
    return true;
}

// Persisted conversation memory: a rolling list of {u,a} pairs on the SD card, reloaded
// into the model's context on boot so Diana remembers past conversations across power cycles.
static void saveConvTurn(const String& u, const String& a) {
    if (!sdOk || u.isEmpty() || a.isEmpty()) return;
    JsonDocument doc;
    File f = SD.open(HISTORY_PATH, FILE_READ);
    if (f) { deserializeJson(doc, f); f.close(); }
    JsonArray arr = doc["t"].is<JsonArray>() ? doc["t"].as<JsonArray>() : doc["t"].to<JsonArray>();
    JsonObject o = arr.add<JsonObject>();
    o["u"] = utf8Truncate(u, 200);
    o["a"] = utf8Truncate(a, 300);
    while ((int)arr.size() > HISTORY_PERSIST) arr.remove(0);
    if (!SD.exists(DIANA_DIR)) SD.mkdir(DIANA_DIR);
    File w = SD.open(HISTORY_PATH, FILE_WRITE);
    if (w) { serializeJson(doc, w); w.close(); }
}

static void loadConvHistory() {
    if (!sdOk) return;
    File f = SD.open(HISTORY_PATH, FILE_READ);
    if (!f) return;
    JsonDocument doc;
    DeserializationError e = deserializeJson(doc, f);
    f.close();
    if (e) return;
    int n = 0;
    for (JsonObject o : doc["t"].as<JsonArray>()) { Gemini.seedTurn(o["u"] | "", o["a"] | ""); n++; }
    if (n) Serial.printf("[MEM] reloaded %d past exchanges\n", n);
}

void appendChatLog(char who, const String& text) {
    if (!sdOk) return;
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.print(Net.timeString("%Y-%m-%d %H:%M"));
    f.print(who == 'u' ? " YOU: " : " DIANA: ");
    f.println(text);
    f.close();
}

String deviceStatusText() {
    String s;
    int bat = M5.Power.getBatteryLevel();
    s += "Battery " + String(bat) + "%";
    if (M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging) s += " (charging)";
    s += ". WiFi " + (Net.isConnected() ? Net.ssid() + " " + String(Net.rssi()) + " dBm, IP " + Net.ip() : String("disconnected"));
    s += ". SD " + (sdOk ? String((uint32_t)(SD.cardSize() / (1024 * 1024))) + " MB" : String("not present"));
    s += ". Free RAM " + String(ESP.getFreeHeap() / 1024) + " KB";
    s += ". Uptime " + String(millis() / 60000) + " min";
    String t = Net.dateTimeString();
    if (t.length()) s += ". Clock " + t;
    s += ". Voice " + String(Config.voiceEnabled && !muted ? "on" : "off") + ". Model " + Config.chatModelOrDefault();
    s += ". Keys " + String(Config.apiKeyCount()) + " (active #" + String(Config.apiKeyIndex) + ").";
    return s;
}

String applyDeviceSetting(const String& settingIn, const String& valueIn) {
    String setting = settingIn; setting.toLowerCase(); setting.trim();
    String value = valueIn; value.toLowerCase(); value.trim();
    bool on = (value == "on" || value == "true" || value == "1" || value == "yes");
    if (setting == "volume") {
        int v = constrain(value.toInt(), 0, 255);
        Config.volume = v; Audio.setVolume(v); Config.save(sdOk);
        return "Volume set to " + String(v) + "/255.";
    }
    if (setting == "brightness") {
        int v = constrain(value.toInt(), 8, 255);
        Config.brightness = v; M5.Display.setBrightness(v); Config.save(sdOk);
        return "Brightness set to " + String(v) + "/255.";
    }
    if (setting == "voice") {
        Config.voiceEnabled = on; Config.save(sdOk);
        UI.setStatus(M5.Power.getBatteryLevel(), false, Net.rssi(), muted || !Config.voiceEnabled, sdOk);
        return String("Voice replies ") + (on ? "enabled." : "disabled.");
    }
    if (setting == "auto_wake") { Config.autoWake = on; Config.save(sdOk); return String("Auto wake ") + (on ? "on." : "off."); }
    if (setting == "boot_music") { Config.bootMusic = on; Config.save(sdOk); return String("Boot music ") + (on ? "on." : "off."); }
    if (setting == "hands_free") {
        Config.handsFree = on; Config.save(sdOk);
        return String("Hands-free listening ") + (on ? "on. Say '" + Config.wakeWord + "' to talk to me." : "off. Press TAB to talk.");
    }
    if (setting == "voice_wake") {
        Config.voiceWake = on; Config.save(sdOk);
        return String("Spoken wake ") + (on ? "on - say 'wake up diana' while I'm asleep." : "off - type the wake phrase instead.");
    }
    if (setting == "mic_gain") { Config.micGain = constrain(value.toInt(), 8, 128); Config.save(sdOk); return "Mic gain " + String(Config.micGain) + " (takes effect after reboot)."; }
    if (setting == "sleep_timeout" || setting == "idle_sleep") {
        Config.idleSleepSec = constrain(value.toInt(), 0, 3600); Config.save(sdOk);
        return Config.idleSleepSec ? "I'll rest after " + String(Config.idleSleepSec) + "s of quiet." : "Auto-sleep off; I'll keep listening.";
    }
    return "Unknown setting: " + settingIn;
}

String addTimer(int seconds, const String& label) {
    if (seconds <= 0 || seconds > TIMER_MAX_SECONDS)
        return "Error: timer must be 1 second to 7 days.";
    for (auto& t : timers) {
        if (!t.active) {
            t.active = true;
            t.endMs = millis() + (uint32_t)seconds * 1000UL;
            t.label = label;
            UI.log("timer set: " + label + " (" + String(seconds) + "s)", 's');
            return "Timer '" + label + "' started for " + String(seconds) + " seconds.";
        }
    }
    return "Error: all 4 timer slots are busy.";
}

void refreshStatusBar() {
    bool charging = M5.Power.isCharging() == m5::Power_Class::is_charging_t::is_charging;
    UI.setStatus(M5.Power.getBatteryLevel(), charging, Net.rssi(), muted || !Config.voiceEnabled, sdOk);
}

static String buildSystemPrompt() {
    String p = FPSTR(DIANA_PROMPT);
    String mem = Memory.formatForPrompt();
    if (mem.length()) { p += "\n"; p += mem; }
    p += "\n[CONTEXT]\n";
    String t = Net.dateTimeString();
    if (t.length()) p += "Current local date/time: " + t + "\n";
    // (owner name intentionally NOT injected for now - she speaks without names)
    if (Net.located()) p += "Device location (approx): " + Net.city() + "\n";
    p += "Voice output is " + String(Config.voiceEnabled && !muted ? "ON" : "OFF") + ". SD card " + (sdOk ? "present" : "absent") + ".\n";
    return p;
}

// key poll used while playing audio so ESC/TAB can interrupt
static bool playbackTick() {
    M5Cardputer.update();
    UI.tick();
    if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
        auto st = M5Cardputer.Keyboard.keysState();
        if (st.esc || st.tab || st.enter) return false;
        for (auto c : st.word) if (c == '`') return false;
    }
    return true;
}

// ── speaking ─────────────────────────────────────────────────────────────────
static bool canSpeak() {
    return Config.voiceEnabled && !muted && sdOk && Net.isConnected();
}

static String g_replyLang;   // language code of the current reply (from the ((mood|lang)) tag)

// Map the reply's language tag (en/fr/es/ne/ja/zh) to a Live speechConfig languageCode so the
// voice speaks with a native accent. Empty -> let the model auto-detect.
static String langToCode(const String& l) {
    String x = l; x.toLowerCase();
    if (x == "fr") return "fr-FR";
    if (x == "es") return "es-ES";
    if (x == "ja" || x == "jp") return "ja-JP";
    if (x == "zh" || x == "cn") return "cmn-CN";
    if (x == "ne") return "ne-NP";
    // English (and untagged) -> no languageCode: the Live default is already English-native, so
    // this keeps ONE stable session for English and avoids reopen churn when the model's tag varies.
    return "";
}

// Map a mood word to a TTS style prompt that keeps Diana's gentle young-android character.
static String moodToStyle(const String& mood) {
    String m = mood; m.toLowerCase();
    if (m == "happy")   return "Say warmly and happily, like a gentle young android: ";
    if (m == "excited") return "Say brightly and excitedly, with youthful energy: ";
    if (m == "calm")    return "Say in a calm, soft, reassuring young voice: ";
    if (m == "gentle")  return "Say gently and tenderly, softly: ";
    if (m == "sad")     return "Say softly and quietly, with a touch of sadness: ";
    if (m == "worried") return "Say in a soft, concerned, caring voice: ";
    if (m == "playful") return "Say playfully, with a light teasing lilt: ";
    if (m == "serious") return "Say clearly and earnestly, in a steady young voice: ";
    if (m == "curious") return "Say with soft, wide-eyed curiosity and wonder: ";
    return "";   // unknown -> use the configured default voice
}

// Extract and remove a leading ((mood)) tag from the reply; returns the mood word (or "").
static String extractMood(String& reply) {
    reply.trim();
    g_replyLang = "";
    if (!reply.startsWith("((")) return "";
    int end = reply.indexOf("))");
    if (end < 0 || end > 24) return "";
    String tag = reply.substring(2, end);
    reply = reply.substring(end + 2);
    reply.trim();
    tag.trim();
    // tag is "mood" or "mood|lang" (e.g. "happy|fr")
    int bar = tag.indexOf('|');
    if (bar >= 0) { g_replyLang = tag.substring(bar + 1); g_replyLang.trim(); tag = tag.substring(0, bar); tag.trim(); }
    return tag;
}

// Speak `text` with streaming TTS: audio starts playing ~1-2 s in, as chunks arrive.
static void speak(const String& text, const String& style = "", const String& lang = "") {
    if (!canSpeak() || text.isEmpty()) return;
    String langCode = langToCode(lang);      // native-accent voice for the reply's language
    String spoken = text;
    spoken = utf8Truncate(spoken, 600);      // keep replies snappy (never split a UTF-8 glyph)

    // MIDNIGHT WHISPER PROTOCOL: after midnight (00:00) until 05:00 Diana speaks in a soft whisper
    // and quieter, so she doesn't blare in the dark. Overrides the emotion style and lowers the
    // volume for this reply, restored afterwards.
    String useStyle = style;                 // REST TTS: per-reply mood prefix
    String liveStyle = "";                    // Live session: only whisper (kept stable so the
                                              // session is reused across turns; mood comes from the words)
    int savedVol = -1;
    int hr = Net.timeValid() ? Net.timeString("%H").toInt() : -1;
    bool whisper = (hr >= 0 && hr < 5);   // 12:00 AM - 4:59 AM
    if (whisper) {
        useStyle = liveStyle = "soft, hushed whisper, almost breathy and very quiet";
        savedVol = Config.volume;
        Audio.setVolume(Config.volume / 3 < 40 ? 40 : Config.volume / 3);
    }

    UI.setState(DianaState::SPEAKING);
    UI.tick();
    uint32_t t0 = millis();
    static uint32_t firstAudioMs = 0;
    firstAudioMs = 0;
    // (streamBegin is called inside each attempt below, so no redundant speaker re-init here)
    auto feed = [](const uint8_t* pcm, size_t len) {
        if (!firstAudioMs) firstAudioMs = millis();
        Audio.streamFeed(pcm, len, playbackTick);
    };
    String err;
    bool ok = false;
    bool quota = false;

    // PRIMARY voice: Gemini Live native-audio (the same model PC Diana uses). It's metered by
    // session, NOT the brutal 10-requests/day cap on the REST TTS preview models, and it speaks
    // many languages naturally. Free the REST TLS socket first so the WebSocket has RAM headroom.
    {
        // Keep the chat TLS alive between turns (next chat reuses it, ~0.8 s faster). Only free it
        // when Live must REOPEN its session (first use / language switch), so the brief moment of
        // two TLS sockets doesn't run the heap low.
        if (!GeminiLive.willReuse(Config.ttsVoiceOrDefault(), liveStyle, langCode, Config.apiKey.c_str()))
            Gemini.disconnect();
        int keys = Config.apiKeyCount() < 1 ? 1 : Config.apiKeyCount();
        for (int ki = 0; ki < keys && !ok; ++ki) {
            String e;
            Audio.streamBegin();
            if (GeminiLive.speak(spoken, Config.ttsVoiceOrDefault(), liveStyle, langCode, Config.apiKey.c_str(), feed, playbackTick, e)) {
                ok = true; Serial.printf("[VOICE] Live native-audio OK (voice=%s)\n", Config.ttsVoiceOrDefault()); break;
            }
            err = e;
            Serial.printf("[VOICE] Live fail (status=%d): %s\n", GeminiLive.lastStatus, e.c_str());
            // Rotate only when the KEY is the problem (auth/quota); a network hiccup must not churn NVS.
            int st = GeminiLive.lastStatus;
            if (Config.apiKeyCount() > 1 && (st == 401 || st == 403 || st == 429)) Config.rotateApiKey();
            else break;
        }
    }
    if (!ok) Serial.println("[VOICE] Live unavailable -> falling back to REST TTS (10/day cap)");

    // FALLBACK only if Live produced no audio: the REST TTS preview models (10/day cap each),
    // rotating keys on a 429 and falling across models.
    if (!ok) {
        static const char* TTS_MODELS[] = {
            "gemini-3.1-flash-tts-preview",
            "gemini-2.5-flash-preview-tts",
            "gemini-2.5-pro-preview-tts",
        };
        for (int mi = 0; mi < 3 && !ok; ++mi) {
            const char* model = (mi == 0) ? Config.ttsModelOrDefault() : TTS_MODELS[mi];
            int keys = Config.apiKeyCount() < 1 ? 1 : Config.apiKeyCount();
            for (int ki = 0; ki < keys && !ok; ++ki) {
                String e;
                Audio.streamBegin();
                if (Gemini.ttsStream(spoken, model, useStyle, feed, playbackTick, e)) { ok = true; break; }
                if (Gemini.lastTtsStatus == 429) { quota = true; err = e; Config.rotateApiKey(); continue; }
                err = e; quota = false; break;      // non-quota error: stop trying
            }
            if (!quota) break;                       // only fall through to other models on quota
        }
    }
    Audio.streamEnd(playbackTick);
    Serial.printf("[TTS] stream ok=%d first-audio=%lums total=%lums underruns=%d\n", ok,
                  (unsigned long)(firstAudioMs ? firstAudioMs - t0 : 0), (unsigned long)(millis() - t0),
                  Audio.lastUnderruns());
    if (savedVol >= 0) Audio.setVolume(savedVol);    // restore volume after a midnight whisper
    if (!ok) UI.log(quota ? "voice: daily voice quota reached (text still works; enable billing for unlimited)"
                          : "voice: " + err, 'w');
    UI.setState(DianaState::IDLE);
}

void speakText(const String& text) { speak(text); }

// ── conversation turn ────────────────────────────────────────────────────────
static void handleReplyText(const String& replyIn, bool fromVoice) {
    String reply = replyIn;
    String style = moodToStyle(extractMood(reply));   // hidden ((mood)) -> voice emotion, stripped
    // voice transcript protocol: ">> what the user said\nreply"
    if (fromVoice && reply.startsWith(">>")) {
        int nl = reply.indexOf('\n');
        String heard = (nl < 0) ? reply.substring(2) : reply.substring(2, nl);
        heard.trim();
        reply = (nl < 0) ? "" : reply.substring(nl + 1);
        reply.trim();
        if (heard.length()) {
            UI.log(heard, 'u');
            appendChatLog('u', heard);
            g_lastUserMsg = heard;
            Gemini.replaceLastUserTurn("[voice message] " + heard);
        }
    }
    if (reply.isEmpty()) return;
    UI.log(reply, 'a');                         // show on screen (instant)
    // Speak FIRST, then persist to SD - the history read+write was adding delay before her voice.
    speak(reply, style, g_replyLang);           // style = mood; g_replyLang = native voice language
    appendChatLog('a', reply);
    saveConvTurn(g_lastUserMsg, reply);         // remember this exchange across reboots (after speaking)
    g_lastUserMsg = "";
}

void runTurn(const String& text, const char* audioPath, size_t audioBytes) {
    if (!Config.loaded) { UI.log("no API key. type /setup or /key <key>", 'e'); return; }
    if (!Net.isConnected()) { UI.log("no WiFi. type /setup or /wifi <ssid> <pass>", 'e'); return; }
    UI.setState(DianaState::THINKING);
    UI.tick();
    // (thinking "umm/hmm" filler removed - user found it annoying)
    Gemini.setSystemPrompt(buildSystemPrompt());

    GeminiResult res;
    bool fromVoice = audioPath != nullptr;
    uint32_t t0 = millis();
    bool ok = Gemini.generate(text, audioPath, audioBytes, res);
    if (!ok && res.status == 503) {           // busy: one retry
        UI.log("neural link busy, retrying...", 's');
        delay(1500);
        ok = Gemini.generate(text, audioPath, audioBytes, res);
    }
    Serial.printf("[GEMINI] turn %lums heap=%u ok=%d status=%d calls=%d%s%s\n",
                  (unsigned long)(millis() - t0), ESP.getFreeHeap(), ok, res.status, (int)res.calls.size(),
                  res.error.length() ? " err=" : "", res.error.c_str());

    for (int round = 0; ; ++round) {
        if (!ok) {
            UI.log(res.error.length() ? res.error : String("neural link error"), 'e');
            Audio.chirpError();
            break;
        }
        if (res.text.length()) handleReplyText(res.text, fromVoice && round == 0);
        if (res.calls.empty()) break;
        if (round >= MAX_TOOL_ROUNDS) { UI.log("tool round limit reached", 'w'); break; }

        // execute tools
        std::vector<FunctionResponse> responses;
        bool needNet = false;
        for (auto& c : res.calls) needNet |= DianaTools::isNetworkTool(c.name);
        if (needNet) Gemini.disconnect();     // free the TLS context before another HTTPS client runs
        for (auto& c : res.calls) {
            UI.log("tool: " + c.name, 's');
            JsonDocument argsDoc;
            deserializeJson(argsDoc, c.argsJson);
            FunctionResponse fr;
            fr.name = c.name;
            fr.id = c.id;
            fr.responseJson = DianaTools::execute(c.name, argsDoc.as<JsonObjectConst>(), hooks);
            responses.push_back(fr);
        }
        UI.setState(DianaState::THINKING);
        UI.tick();
        ok = Gemini.sendFunctionResponses(responses, res);
        fromVoice = false;
    }
    UI.setState(DianaState::IDLE);
    if (shutdownArmed) {
        UI.log("Barnyard Protocol: powering down.", 's');
        UI.tick();
        delay(800);
        Audio.toneMs(880, 80); Audio.toneMs(660, 80); Audio.toneMs(440, 200);
        M5.Display.fillScreen((uint32_t)C_BLACK);
        M5.Display.setBrightness(0);
        Gemini.disconnect();
        WiFi.disconnect(true);
        delay(200);
        M5.Power.powerOff();          // deep sleep on Cardputer; power switch / reset wakes it
        esp_deep_sleep_start();
    }
}

// ── welcome protocol (music -> chirp -> "Diana fully functional" -> weather) ──
void welcomeProtocol() {
    awake = true;
    UI.showHud();
    UI.setState(DianaState::WAKING);
    UI.log("DIANA awakened.", 's');
    refreshStatusBar();
    UI.tick();

    // Boot music (or a chirp), then the greeting streams and speaks live.
    bool music = sdOk && Config.bootMusic && SD.exists(BOOT_WAV_PATH);
    if (music) {
        UI.setState(DianaState::WAKING);
        UI.tick();
        Audio.playWavFile(BOOT_WAV_PATH, playbackTick, Config.volume * 3 / 4);
    } else {
        Audio.chirpBoot();
        Audio.chirpWake();
    }

    int hour = Net.timeValid() ? Net.timeString("%H").toInt() : -1;
    String daypart = hour < 0 ? "" : hour < 5 ? "Good night" : hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
    // Wake-up call: "Hello. <time greeting>. I am Diana, D-I-0336-7. Happy to help you." (no user name, per request)
    String greet = "Hello.";
    if (daypart.length()) greet += " " + daypart + ".";
    greet += " I am Diana, " DIANA_ID ". Happy to help you.";
    UI.log(greet, 'a');
    speak(greet);   // spoken via the Live voice (generous quota); whispers automatically at night
    UI.setState(DianaState::IDLE);
}

void enterStandby() {
    awake = false;
    if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }
    Gemini.disconnect();
    GeminiLive.end();                 // free the second TLS socket while asleep
    UI.showStandby();
}

// Open the credentials portal: drop both TLS sockets first so the AP + web server have heap.
void openSetupPortal() {
    UI.log("starting setup portal...", 's');
    Gemini.disconnect();
    GeminiLive.end();
    WiFi.disconnect(true);
    delay(100);
    if (Setup.start()) UI.showSetup(SETUP_AP_SSID, Setup.psk(), Setup.url(), 0);
    else { UI.log("portal failed", 'e'); if (awake) UI.showHud(); else UI.showStandby(); }
}

// ── input submission ─────────────────────────────────────────────────────────
static void submitInput() {
    String text = input;
    input = "";
    UI.setInput(input);
    text.trim();
    if (text.isEmpty()) return;
    if (handleCommand(text)) return;
    String lower = text; lower.toLowerCase();
    if (!awake) {
        if (isWakePhrase(lower)) welcomeProtocol();
        return;
    }
    UI.log(text, 'u');
    appendChatLog('u', text);
    g_lastUserMsg = text;
    if (lower == "goodnight diana" || lower.indexOf("barnyard protocol") >= 0) shutdownArmed = true;
    runTurn(text, nullptr, 0);
}

static void startVoice() {
    if (!sdOk) { UI.log("voice input needs an SD card", 'w'); return; }
    if (!Net.isConnected()) { UI.log("no WiFi", 'e'); return; }
    if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }
    // Hold-to-talk: no silence auto-stop; the TAB release ends the recording.
    if (!Audio.startRecording(REC_PATH, false, Config.vadThreshold, Config.silenceMs)) {
        UI.log("mic start failed", 'e');
        return;
    }
    Audio.chirpAck();
    UI.setState(DianaState::RECORDING);
}

static void finishVoice() {
    size_t bytes = Audio.stopRecording();
    UI.setState(DianaState::THINKING);
    UI.tick();
    if (bytes < REC_RATE * 2 / 2) {           // < 0.5 s
        UI.log("(too short)", 's');
        UI.setState(DianaState::IDLE);
        return;
    }
    if (!Audio.heardSpeech() && Config.autoStop) {
        UI.log("(I didn't hear anything)", 's');
        UI.setState(DianaState::IDLE);
        return;
    }
    UI.log("[voice " + String(bytes / (REC_RATE * 2.0f), 1) + "s]", 'u');
    runTurn("(voice message from the microphone)", REC_PATH, bytes);
}

// ── keyboard ─────────────────────────────────────────────────────────────────
static void handleKeys() {
    if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
    auto st = M5Cardputer.Keyboard.keysState();

    if (!awake && UI.state() == DianaState::STANDBY) {
        // Local wake: she only activates when you type the wake phrase (or a /command).
        if (st.enter) {
            String t = input; input = ""; t.trim();
            String low = t; low.toLowerCase();
            if (t.startsWith("/")) { handleCommand(t); if (UI.state() == DianaState::STANDBY) UI.setStandbyInput(""); }
            else if (isWakePhrase(low)) { UI.showHud(); welcomeProtocol(); }
            else { UI.setStandbyInput(""); }
        } else if (st.backspace || st.del) {
            if (input.length()) { utf8Backspace(input); UI.setStandbyInput(input); }
        } else {
            bool changed = false;
            for (auto c : st.word) if ((unsigned char)c >= 0x20) { input += c; changed = true; }
            if (changed) UI.setStandbyInput(input);
        }
        return;
    }

    // (RECORDING is handled in loop() before handleKeys() is reached)
    if (st.fn) {
        if (st.up) UI.scroll(+3);
        if (st.down) UI.scroll(-3);
        if (st.esc) { if (input.length()) { input = ""; UI.setInput(input); } else enterStandby(); }
        if (st.del) { input = ""; UI.setInput(input); }
        return;
    }
    if (st.tab) { startVoice(); return; }
    if (st.enter) { submitInput(); return; }
    if (st.backspace || st.del) {
        if (input.length()) { utf8Backspace(input); UI.setInput(input); }
        return;
    }
    bool changed = false;
    for (auto c : st.word) {
        if (c == '`' && input.isEmpty()) { UI.scroll(-1000); continue; }   // bare ` on empty input: jump to newest
        if ((unsigned char)c >= 0x20) { input += c; changed = true; }
    }
    if (changed) UI.setInput(input);
}

// ── timers ───────────────────────────────────────────────────────────────────
static bool timerDue() {
    uint32_t now = millis();
    for (auto& t : timers) if (t.active && (int32_t)(now - t.endMs) >= 0) return true;
    return false;
}

static void serviceTimers() {
    uint32_t now = millis();
    for (auto& t : timers) {
        if (t.active && (int32_t)(now - t.endMs) >= 0) {
            t.active = false;
            UI.log("timer done: " + t.label, 'w');
            Audio.chirpTimer();
            if (awake && Net.isConnected()) {
                runTurn("[SYSTEM EVENT] The timer '" + t.label + "' just finished. Tell the user briefly.", nullptr, 0);
            }
        }
    }
}

// ── setup / loop ─────────────────────────────────────────────────────────────
void setup() {
    auto cfg = M5.config();
    cfg.internal_mic = true;
    cfg.internal_spk = true;
    M5Cardputer.begin(cfg, true);
    Serial.begin(115200);
    delay(50);
    const char* rr;
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  rr = "power-on"; break;
        case ESP_RST_SW:       rr = "software"; break;
        case ESP_RST_PANIC:    rr = "PANIC (code crash)"; break;
        case ESP_RST_INT_WDT:  rr = "interrupt-WDT"; break;
        case ESP_RST_TASK_WDT: rr = "task-WDT (hang)"; break;
        case ESP_RST_WDT:      rr = "WDT"; break;
        case ESP_RST_BROWNOUT: rr = "BROWNOUT (power dip)"; break;
        case ESP_RST_DEEPSLEEP:rr = "deep-sleep wake"; break;
        default:               rr = "other"; break;
    }
    Serial.printf("\n[DIANA] %s v%s booting, heap=%u, reset=%s\n", DIANA_ID, DIANA_VERSION, ESP.getFreeHeap(), rr);

    UI.begin(false, 160);
    UI.showBoot();
    bootLog("Neural core online");
    bootLog(String("Board: ") + (M5.getBoard() == m5::board_t::board_M5CardputerADV ? "Cardputer ADV" : "Cardputer"));

    sdOk = mountSd();
    bootLog(sdOk ? "microSD: " + String((uint32_t)(SD.cardSize() / (1024 * 1024))) + " MB" : "microSD: none (text only)", sdOk);

    bool haveCfg = Config.load(sdOk);
    bootLog(haveCfg ? String("Config: ") + (Config.fromSd ? "SD card" : "internal") : "Config: missing -> setup", haveCfg);
    bootLog("API keys: " + String(Config.apiKeyCount()) + " (active #" + String(Config.apiKeyIndex) + ")");
    UI.setFont(Config.font != "ascii");   // multilingual efont (EN/FR/ES/JP) unless user forces plain ASCII
    M5.Display.setBrightness(Config.brightness);
    Audio.begin(Config.volume);
    // (boot self-test tone removed: the loud transient could brown out the 3.3V rail on
    //  battery and reset the device right at wake. Use /beep to test the speaker manually.)

    Memory.begin(sdOk);
    loadConvHistory();            // reload past conversation so she remembers across reboots
    bootLog("Memory: " + String(Memory.count()) + " facts, history reloaded");

    // Forth: tunables and scripts from the SD card, no reflash needed (/diana/boot.fs runs now)
    if (Forth.begin()) {
        int n = (sdOk && SD.exists(Forth.bootScript())) ? Forth.runFile(Forth.bootScript(), false) : 0;
        bootLog(n ? "Forth: boot.fs (" + String(n) + " lines)" : "Forth: ready");
    } else bootLog("Forth: init failed", false);

    hooks.webSearch = [](const String& q) {
        String ans, err;
        return Gemini.groundedSearch(q, ans, err) ? ans : ("Error: " + err);
    };
    hooks.setMuted = [](bool m) { muted = m; refreshStatusBar(); };
    hooks.requestShutdown = []() { shutdownArmed = true; };
    hooks.addTimer = addTimer;
    hooks.deviceStatus = deviceStatusText;
    hooks.deviceSetting = applyDeviceSetting;
    hooks.connectWifi = [](const String& ssid, const String& pass) -> String {
        if (ssid.isEmpty()) return "Error: I need the network name.";
        Config.setWifi(ssid, pass);
        Config.save(sdOk);
        Gemini.disconnect();
        bool ok = Net.connect(9000, nullptr);
        if (ok) Net.syncTime(Config.tz);
        return ok ? "Connected to " + ssid + " at " + Net.ip() + "."
                  : "Couldn't connect to " + ssid + " (wrong password or out of range).";
    };
    hooks.scanWifi = []() -> String {
        Gemini.disconnect();
        int n = WiFi.scanNetworks(false, true);
        if (n <= 0) return "No WiFi networks found nearby.";
        int chCount[15] = {0}, hidden = 0, open = 0;
        for (int i = 0; i < n; ++i) {
            int ch = WiFi.channel(i);
            if (ch >= 1 && ch <= 14) chCount[ch]++;
            if ((int)WiFi.encryptionType(i) == 0) open++;
            if (WiFi.SSID(i).isEmpty()) hidden++;
        }
        int busy = 1; for (int c = 2; c <= 14; ++c) if (chCount[c] > chCount[busy]) busy = c;
        String s = String(n) + " access points; busiest channel " + busy + "; " + hidden + " hidden, " + open + " open. Strongest: ";
        for (int i = 0; i < n && i < 6; ++i) { if (i) s += ", "; s += (WiFi.SSID(i).isEmpty() ? String("(hidden)") : WiFi.SSID(i)) + " ch" + WiFi.channel(i) + " " + WiFi.RSSI(i) + "dBm"; }
        WiFi.scanDelete();
        return s;
    };
    hooks.whereAmI = []() -> String {
        if (!Net.isConnected()) return "I'm offline, so I can't locate us.";
        if (Net.locate()) return "We seem to be near " + Net.city() + " - approximate, from the internet connection. This pocket body has no GPS chip.";
        return "I couldn't work out our location.";
    };
    hooks.openSettings = []() { menuOpenReq = true; };
    hooks.musicPlay = [](const String& track) -> String {
        musicReq = true; musicTrack = track;
        return track.length() ? ("Playing " + track + " on pixel music.") : "Opening pixel music.";
    };

    if (!Config.hasWifi() || !Config.loaded) {
        bootLog("Opening setup portal...");
        delay(600);
        if (Setup.start()) UI.showSetup(SETUP_AP_SSID, Setup.psk(), Setup.url(), 0);
        else bootLog("Setup portal failed to start", false);
        return;
    }

    bool net = Net.connect(9000, progressCb);
    bootLog(net ? "WiFi: " + Net.ip() : "WiFi: failed (use /setup)", net);
    if (net) {
        bool ts = Net.syncTime(Config.tz);
        bootLog(ts ? "Clock: " + Net.timeString("%H:%M") : "Clock: not synced", ts);
        if (sdOk) {
            File ca = SD.open("/diana/ca.pem", FILE_READ);
            if (ca) { String pem = ca.readString(); ca.close(); if (pem.length() > 100) { HttpsStream::setCACert(pem); bootLog("TLS: using ca.pem"); } }
        }
    }
    bootLog(String("Model: ") + Config.chatModelOrDefault());
    Audio.chirpAck();
    delay(700);
    welcomeProtocol();   // PTT mode: boot awake & idle, ready for hold-TAB-to-talk (no listening standby)
    // (thinking-filler generation removed - user found the "umm/hmm" annoying)
}

void loop() {
    M5Cardputer.update();
    serviceSerialConsole();

    // Pixel music player (opened by /music or the music_play voice tool)
    if (musicReq && !Music.isOpen()) {
        musicReq = false;
        Music.open(0);
        if (musicTrack.length()) Music.playByName(musicTrack);
        musicTrack = "";
    }
    if (Music.isOpen()) {
        Music.handleKeysRaw();
        if (Music.isOpen()) Music.service();
        else { if (awake) UI.showHud(); else UI.showStandby(); }
        delay(4);
        return;
    }

    // Settings menu (opened by /settings, the standby menu item, or the voice tool)
    if (menuOpenReq && !Menu.isOpen()) { menuOpenReq = false; Menu.open(); }
    if (Menu.isOpen()) {
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())
            Menu.handleKeys(M5Cardputer.Keyboard.keysState());
        if (Menu.isOpen()) {
            Menu.tick();
        } else if (Menu.takeSetupRequest()) {
            openSetupPortal();
        } else {
            if (awake) UI.showHud();
            else UI.showStandby();
        }
        delay(5);
        return;
    }

    if (UI.state() == DianaState::SETUP) {
        static uint32_t lastClients = 0;
        static int shownClients = -1;
        if (!Setup.loop()) {
            if (Setup.saved()) { delay(300); ESP.restart(); }   // credentials saved -> reboot into them
            else { UI.log("setup portal stopped", 'w'); enterStandby(); return; }   // never treat "not running" as "saved"
        }
        if (millis() - lastClients > 1000) {
            lastClients = millis();
            int c = Setup.clients();
            if (c != shownClients) { shownClients = c; UI.showSetup(SETUP_AP_SSID, Setup.psk(), Setup.url(), c); }
        }
        if (M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed()) {
            auto st = M5Cardputer.Keyboard.keysState();
            if (st.esc || st.enter) {
                Setup.stop();
                if (Config.hasWifi()) { Net.connect(9000, nullptr); Net.syncTime(Config.tz); }
                enterStandby();
            }
        }
        delay(5);
        return;
    }

    if (UI.state() == DianaState::RECORDING) {
        // Hold-to-talk: record while TAB is held; release to send. ESC cancels.
        UI.setLevel(Audio.level());
        auto ks = M5Cardputer.Keyboard.keysState();
        if (ks.esc) { Audio.stopRecording(); UI.setState(DianaState::IDLE); UI.log("(cancelled)", 's'); UI.tick(); return; }
        bool held  = ks.tab;                 // refreshed every loop by M5Cardputer.update()
        bool going = Audio.pollRecording();  // false = hit max length
        if (!held || !going) finishVoice();  // TAB released (or max length) -> send
        UI.tick();
        return;
    }

    // HANDS-FREE (tight loop, mirrors the chop-free PTT branch): while enabled she listens
    // continuously in a MINIMAL loop that returns early - none of the heavy per-loop work
    // (keepalive pings, status-bar redraws, WiFi retries) runs while the mic is live. That heavy
    // work during listening is what coupled the "chop" into the amp; PTT stays tight and is clean,
    // so this mirrors it. No wake word: she just listens and answers.
    if (Config.handsFree && awake && UI.state() == DianaState::IDLE && sdOk && Net.isConnected()
        && input.isEmpty() && !shutdownArmed && !M5Cardputer.Keyboard.isPressed()) {
        if (!Audio.isListening()) Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs);
        DianaAudio::ListenResult r = Audio.pollListening();
        UI.setListening(true, Audio.level());
        UI.tick();
        if (r == DianaAudio::LISTEN_DONE) {
            size_t bytes = Audio.takeUtterance();
            if (bytes >= 24000 && Audio.listenHeardSpeech()) {   // ~0.75s of real speech
                Audio.stopListening(); UI.setListening(false, 0);
                UI.setState(DianaState::THINKING); UI.tick();
                runTurn("(voice message from the microphone)", REC_PATH, bytes);
                UI.setState(DianaState::IDLE);
            }
        }
        if (!timerDue()) return;   // tight loop: skip the heavy tail while the mic is live (that's the chop fix)
        Audio.stopListening(); UI.setListening(false, 0);   // a timer fired: fall through so serviceTimers() runs
    }
    if (Config.handsFree && Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }

    handleKeys();
    serviceTimers();
    if (awake && Net.isConnected() && !Audio.isListening()) GeminiLive.keepAlive();

    if (millis() - lastStatusMs > 2000) {
        lastStatusMs = millis();
        refreshStatusBar();
        if (awake && Net.isConnected() == false && Config.hasWifi()) {
            static uint32_t lastRetry = 0;
            if (millis() - lastRetry > 30000) { lastRetry = millis(); Net.connect(6000, nullptr); }
        }
    }
    if (!consoleUiFrozen()) UI.tick();
    delay(5);
}
