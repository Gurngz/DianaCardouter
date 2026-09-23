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

// The TLS handshake, base64 streaming buffers and JSON parsing all run on the
// loop task; the default 8 KB stack is too tight for that chain.
SET_LOOP_TASK_STACK_SIZE(20 * 1024);

// ── app state ────────────────────────────────────────────────────────────────
static bool   sdOk = false;
static bool   muted = false;
static bool   shutdownArmed = false;
static bool   awake = false;
static String input;
static bool   ttsAvailable = false;      // voice replies need SD (audio is streamed through it)
static uint32_t lastStatusMs = 0;
static uint32_t idleSinceMs = 0;

// serial-console diagnostics state (declared early so the service loop can see them)
static int      g_hunt = -1;              // audio-source hunt step (-1 = idle)
static uint32_t g_huntAt = 0;
static bool     g_listenPaused = false;   // !pause: hard-stop listening (mic off) w/o the loop restarting it
static bool     g_freezeUI = false;       // !ui off: skip all display redraws (isolate screen-coupling from mic)
static uint32_t lastInteractionMs = 0;   // for the hands-free follow-up window
static bool   g_gateWake = false;        // current turn must contain the wake word to be answered
static bool   g_turnSuppressed = false;  // set when a hands-free turn was ignored (not addressed)
static bool   menuOpenReq = false;       // voice tool asked to open the settings menu
static bool   g_musicReq = false;        // voice tool asked to open pixel music
static String g_musicTrack;

struct DianaTimer { bool active = false; uint32_t endMs = 0; String label; };
static DianaTimer timers[4];

static ToolHooks hooks;

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

static String g_lastUserMsg;   // this turn's user message, paired with the reply when persisted

// Persisted conversation memory: a rolling list of {u,a} pairs on the SD card, reloaded
// into the model's context on boot so Diana remembers past conversations across power cycles.
static void saveConvTurn(const String& u, const String& a) {
    if (!sdOk || u.isEmpty() || a.isEmpty()) return;
    JsonDocument doc;
    File f = SD.open(HISTORY_PATH, FILE_READ);
    if (f) { deserializeJson(doc, f); f.close(); }
    JsonArray arr = doc["t"].is<JsonArray>() ? doc["t"].as<JsonArray>() : doc["t"].to<JsonArray>();
    JsonObject o = arr.add<JsonObject>();
    o["u"] = u.substring(0, 200);
    o["a"] = a.substring(0, 300);
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

static void appendChatLog(char who, const String& text) {
    if (!sdOk) return;
    File f = SD.open(LOG_PATH, FILE_APPEND);
    if (!f) return;
    f.print(Net.timeString("%Y-%m-%d %H:%M"));
    f.print(who == 'u' ? " YOU: " : " DIANA: ");
    f.println(text);
    f.close();
}

static String deviceStatusText() {
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

static String applyDeviceSetting(const String& settingIn, const String& valueIn) {
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

static String addTimer(int seconds, const String& label) {
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

static void refreshStatusBar() {
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
    if (Config.userName.length()) p += "The device owner's name is " + Config.userName + ".\n";
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
    return Config.voiceEnabled && !muted && ttsAvailable && Net.isConnected();
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
    if (!reply.startsWith("((")) return "";
    int end = reply.indexOf("))");
    if (end < 0 || end > 20) return "";
    String mood = reply.substring(2, end);
    reply = reply.substring(end + 2);
    reply.trim();
    mood.trim();
    return mood;
}

// Speak `text` with streaming TTS: audio starts playing ~1-2 s in, as chunks arrive.
static void speak(const String& text, const String& style = "") {
    if (!canSpeak() || text.isEmpty()) return;
    String spoken = text;
    if (spoken.length() > 600) spoken = spoken.substring(0, 600);   // keep replies snappy

    // MIDNIGHT WHISPER PROTOCOL: after midnight (00:00) until 05:00 Diana speaks in a soft whisper
    // and quieter, so she doesn't blare in the dark. Overrides the emotion style and lowers the
    // volume for this reply, restored afterwards.
    String useStyle = style;
    int savedVol = -1;
    int hr = Net.timeValid() ? Net.timeString("%H").toInt() : -1;
    bool whisper = (hr >= 0 && hr < 5);   // 12:00 AM - 4:59 AM
    if (whisper) {
        useStyle = "soft, hushed whisper, almost breathy and very quiet";
        savedVol = Config.volume;
        Audio.setVolume(Config.volume / 3 < 40 ? 40 : Config.volume / 3);
    }

    UI.setState(DianaState::SPEAKING);
    UI.tick();
    uint32_t t0 = millis();
    static uint32_t firstAudioMs = 0;
    firstAudioMs = 0;
    Audio.streamBegin();
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
        Gemini.disconnect();
        int keys = Config.apiKeyCount() < 1 ? 1 : Config.apiKeyCount();
        for (int ki = 0; ki < keys && !ok; ++ki) {
            String e;
            Audio.streamBegin();
            if (GeminiLive.speak(spoken, Config.ttsVoiceOrDefault(), useStyle, Config.apiKey.c_str(), feed, playbackTick, e)) {
                ok = true; Serial.printf("[VOICE] Live native-audio OK (voice=%s)\n", Config.ttsVoiceOrDefault()); break;
            }
            err = e;
            Serial.printf("[VOICE] Live fail (status=%d): %s\n", GeminiLive.lastStatus, e.c_str());
            if (Config.apiKeyCount() > 1) Config.rotateApiKey();
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

        // Hands-free wake-word gate: only answer if the wake word was spoken,
        // or we're still inside the follow-up window after Diana just replied.
        if (g_gateWake) {
            String h = heard; h.toLowerCase();
            String ww = Config.wakeWord; ww.toLowerCase();
            bool addressed = ww.length() && h.indexOf(ww) >= 0;
            bool followup = lastInteractionMs && (millis() - lastInteractionMs) < FOLLOWUP_MS;
            if (!addressed && !followup) {
                Gemini.rollbackLastUserTurn();          // leave no trace in context
                if (heard.length()) UI.log("(heard, not addressed: " + heard + ")", 's');
                g_turnSuppressed = true;
                return;
            }
        }
        if (heard.length()) {
            UI.log(heard, 'u');
            appendChatLog('u', heard);
            g_lastUserMsg = heard;
            Gemini.replaceLastUserTurn("[voice message] " + heard);
        }
    }
    if (reply.isEmpty()) return;
    UI.log(reply, 'a');
    appendChatLog('a', reply);
    saveConvTurn(g_lastUserMsg, reply);         // remember this exchange across reboots
    g_lastUserMsg = "";
    lastInteractionMs = millis();
    speak(reply, style);                        // style carries the chosen mood
    lastInteractionMs = millis();               // reset after speaking so the window covers the pause
}

static void runTurn(const String& text, const char* audioPath, size_t audioBytes) {
    if (!Config.loaded) { UI.log("no API key. type /setup or /key <key>", 'e'); return; }
    if (!Net.isConnected()) { UI.log("no WiFi. type /setup or /wifi <ssid> <pass>", 'e'); return; }
    UI.setState(DianaState::THINKING);
    UI.tick();
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

    for (int round = 0; round < 4; ++round) {
        if (!ok) {
            UI.log(res.error.length() ? res.error : String("neural link error"), 'e');
            Audio.chirpError();
            break;
        }
        if (res.text.length()) handleReplyText(res.text, fromVoice && round == 0);
        if (res.calls.empty()) break;

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
static void welcomeProtocol() {
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

    String name = Config.userName.length() ? Config.userName : Memory.userName();
    int hour = Net.timeValid() ? Net.timeString("%H").toInt() : -1;
    String daypart = hour < 0 ? "" : hour < 5 ? "Good night" : hour < 12 ? "Good morning" : hour < 18 ? "Good afternoon" : "Good evening";
    // Wake-up call: "Hello. <time greeting>, <name>. I am Diana, D-I-0336-7. Happy to help you."
    String greet = "Hello.";
    if (daypart.length()) greet += " " + daypart + (name.length() ? ", " + name : "") + ".";
    else if (name.length()) greet += " " + name + ".";
    greet += " I am Diana, " DIANA_ID ". Happy to help you.";
    UI.log(greet, 'a');
    speak(greet);   // spoken via the Live voice (generous quota); whispers automatically at night
    UI.setState(DianaState::IDLE);
    idleSinceMs = millis();
    lastInteractionMs = millis();
}

static void enterStandby() {
    awake = false;
    if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }
    Gemini.disconnect();
    UI.showStandby();
}

// While asleep: listen for the spoken wake phrase. On a captured utterance, do a cheap
// transcription-only call and wake if it contains "wake ... diana". Typed wake still works.
static void serviceStandbyWake() {
    bool want = !awake && UI.state() == DianaState::STANDBY && Config.voiceWake && Config.handsFree
                && sdOk && Net.isConnected() && input.isEmpty();
    if (!want) {
        if (Audio.isListening()) Audio.stopListening();
        return;
    }
    if (!Audio.isListening()) {
        if (!Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs)) return;
    }
    static uint32_t lastWakeTry = 0;           // throttle transcribe attempts (quota + click)
    DianaAudio::ListenResult r = Audio.pollListening();
    if (r == DianaAudio::LISTEN_DONE) {
        // Keep the mic running (no end/begin -> no click) and grab the utterance.
        size_t bytes = Audio.takeUtterance();
        // Only spend a paid transcribe call on a clearly-loud, long-enough utterance, and
        // no more than once every 2 s. Room-noise hallucinations never clear this bar.
        bool loudEnough = bytes >= 40000 && Audio.listenHeardSpeech();
        bool cooled = (millis() - lastWakeTry) > 2000;
        if (loudEnough && cooled) {
            lastWakeTry = millis();
            UI.setStandbyInput("(heard something...)");
            String heard, err;
            bool ok = Gemini.transcribe(REC_PATH, bytes, heard, err);
            Serial.printf("[WAKE] heard: '%s' (ok=%d, bytes=%u)\n", heard.c_str(), ok, (unsigned)bytes);
            String h = heard; h.toLowerCase();
            // Speech-to-text often mangles "Diana" (tana/dana/deanna...), so match loosely:
            // any of these, or the phrase "wake"/"awake", wakes her.
            bool wake = ok && (h.indexOf("wake") >= 0 || h.indexOf("awake") >= 0 ||
                               h.indexOf("diana") >= 0 || h.indexOf("dana") >= 0 || h.indexOf("tana") >= 0 ||
                               h.indexOf("deanna") >= 0 || h.indexOf("hey diana") >= 0 || h.indexOf("hey") >= 0);
            if (wake) { Audio.stopListening(); UI.showHud(); welcomeProtocol(); return; }
            UI.setStandbyInput("");
        }
    }
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

static bool handleCommand(const String& lineIn) {
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
    else if (cmd == "/music") { g_musicReq = true; g_musicTrack = arg; }
    else if (cmd == "/setup") { UI.log("starting setup portal...", 's'); Gemini.disconnect(); WiFi.disconnect(true); delay(100); if (Setup.start()) { UI.showSetup(SETUP_AP_SSID, Setup.url(), 0); } else UI.log("portal failed", 'e'); }
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
    else if (cmd == "/delkey") { int n = arg.toInt(); if (Config.removeApiKey(n)) { Config.save(sdOk); UI.log("removed key #" + String(n) + " (" + Config.apiKeyCount() + " left)", 's'); } else UI.log("no key #" + String(n) + " (have " + Config.apiKeyCount() + ")", 'w'); }
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
        int s1 = arg.indexOf(' '); String proto = s1 < 0 ? arg : arg.substring(0, s1); String rest = s1 < 0 ? "" : arg.substring(s1 + 1);
        rest.trim(); int s2 = rest.indexOf(' ');
        uint32_t a = (uint32_t)strtoul((s2 < 0 ? String("0") : rest.substring(0, s2)).c_str(), nullptr, 0);
        uint32_t c = (uint32_t)strtoul((s2 < 0 ? rest : rest.substring(s2 + 1)).c_str(), nullptr, 0);
        UI.log(dianaIrSend(proto.length() ? proto : String("nec"), a, c), 's');
    }
    else if (cmd == "/irsave") {
        // /irsave <name>|<proto> <addr> <cmd>   e.g. /irsave projector power|nec 0 0x1234
        int bar = arg.indexOf('|');
        String nm = bar < 0 ? arg : arg.substring(0, bar);
        String rest = bar < 0 ? "" : arg.substring(bar + 1); rest.trim();
        int a1 = rest.indexOf(' '); String pr = a1 < 0 ? rest : rest.substring(0, a1); rest = a1 < 0 ? "" : rest.substring(a1 + 1); rest.trim();
        int a2 = rest.indexOf(' ');
        uint32_t ad = (uint32_t)strtoul((a2 < 0 ? String("0") : rest.substring(0, a2)).c_str(), nullptr, 0);
        uint32_t cc = (uint32_t)strtoul((a2 < 0 ? rest : rest.substring(a2 + 1)).c_str(), nullptr, 0);
        UI.log(dianaIrSaveNamed(nm, pr.length() ? pr : String("nec"), ad, cc), 's');
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
    else if (cmd == "/model") { Config.chatModel = arg; Config.save(sdOk); UI.log(String("chat model: ") + Config.chatModelOrDefault(), 's'); }
    else if (cmd == "/think") { Config.thinkingLevel = arg; Config.save(sdOk); UI.log("thinking level: " + (arg.length() ? arg : String("(model default)")), 's'); }
    else if (cmd == "/ttsmodel") { Config.ttsModel = arg; Config.save(sdOk); UI.log(String("tts model: ") + Config.ttsModelOrDefault(), 's'); }
    else if (cmd == "/sleep") { enterStandby(); }
    else if (cmd == "/reboot") { ESP.restart(); }
    else if (cmd == "/off") { shutdownArmed = true; runTurn("Barnyard Protocol.", nullptr, 0); }
    else if (cmd == "/heap") { UI.log("free heap " + String(ESP.getFreeHeap()) + " largest " + String(ESP.getMaxAllocHeap()), 's'); }
    else { UI.log("unknown command. /help", 'w'); }
    return true;
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
        if (lower.indexOf("wake up") >= 0) welcomeProtocol();
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
    lastInteractionMs = millis();
}

// Continuous hands-free listening: whenever Diana is idle she keeps an ear open,
// and answers when she hears her wake word (or during the follow-up window).
static void serviceHandsFree() {
    if (!awake) return;                         // standby voice-wake owns the mic while asleep
    bool want = Config.handsFree && sdOk && Net.isConnected()
                && input.isEmpty() && !shutdownArmed && UI.state() == DianaState::IDLE;
    if (!want) {
        if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }
        return;
    }
    if (!Audio.isListening()) {
        if (!Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs)) return;
    }
    DianaAudio::ListenResult r = Audio.pollListening();
    // Throttle the level-meter redraw to ~5 Hz. Redrawing it every mic chunk (~30 Hz) fires a
    // periodic display-SPI/rail burst that can couple into the (always-on) speaker amp as a
    // rhythmic "helicopter" chop while she listens. 5 Hz still looks live but is far quieter.
    static uint32_t lastMeter = 0;
    if (!g_freezeUI && millis() - lastMeter > 200) { lastMeter = millis(); UI.setListening(true, Audio.level()); }
    if (r == DianaAudio::LISTEN_DONE) {
        // Grab the utterance but keep the mic running (no end/begin -> no click).
        size_t bytes = Audio.takeUtterance();
        // Only spend a paid chat turn on a clearly-loud, long-enough utterance. Room-noise
        // hallucinations stay below this bar, so she quietly keeps listening (no click, no quota).
        if (bytes >= 24000 && Audio.listenHeardSpeech()) {   // ~0.75 s of real speech
            Audio.stopListening();                  // free the mic; the turn uses the speaker
            UI.setListening(false, 0);
            UI.setState(DianaState::THINKING);
            UI.tick();
            // Once she's awake, answer whatever she hears — no per-sentence wake word
            // (speech-to-text mangles "Diana" too often to gate on it reliably).
            g_gateWake = false;
            g_turnSuppressed = false;
            runTurn("(voice message from the microphone)", REC_PATH, bytes);
            UI.setState(DianaState::IDLE);
        }
        // listening continues automatically on the next loop
    }
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
            else if (low.indexOf("wake up diana") >= 0 || low.indexOf("diana wake up") >= 0) { UI.showHud(); welcomeProtocol(); }
            else { UI.setStandbyInput(""); }
        } else if (st.backspace || st.del) {
            if (input.length()) {
                int i = input.length() - 1;
                while (i > 0 && ((unsigned char)input[i] & 0xC0) == 0x80) --i;
                input = input.substring(0, i);
                UI.setStandbyInput(input);
            }
        } else {
            bool changed = false;
            for (auto c : st.word) if ((unsigned char)c >= 0x20) { input += c; changed = true; }
            if (changed) UI.setStandbyInput(input);
        }
        return;
    }

    if (UI.state() == DianaState::RECORDING) {
        if (st.tab || st.enter) finishVoice();
        else if (st.esc) { Audio.stopRecording(); UI.setState(DianaState::IDLE); UI.log("(cancelled)", 's'); }
        return;
    }

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
        if (input.length()) {
            // remove one UTF-8 glyph
            int i = input.length() - 1;
            while (i > 0 && ((unsigned char)input[i] & 0xC0) == 0x80) --i;
            input = input.substring(0, i);
            UI.setInput(input);
        }
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
    ttsAvailable = sdOk;

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
        g_musicReq = true; g_musicTrack = track;
        return track.length() ? ("Playing " + track + " on pixel music.") : "Opening pixel music.";
    };

    if (!Config.hasWifi() || !Config.loaded) {
        bootLog("Opening setup portal...");
        delay(600);
        if (Setup.start()) UI.showSetup(SETUP_AP_SSID, Setup.url(), 0);
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
}

// ── serial debug console ──────────────────────────────────────────────────────
// Lets me drive Diana over USB for a remote check-up: wake her, talk, and probe /
// toggle the ES8311 audio registers live while the user listens for the "helicopter"
// chop, so we can bisect its source without a reflash per guess.
static void es8311Dump() {
    const uint8_t regs[] = {0x00,0x01,0x02,0x0D,0x0E,0x12,0x13,0x14,0x17,0x1C,0x31,0x32,0x37,0x45};
    Serial.print("[ES8311]");
    for (uint8_t r : regs) Serial.printf(" %02X=%02X", r, M5.In_I2C.readRegister8(0x18, r, 100000));
    Serial.println();
}

// Additive source-elimination while she LISTENS: each step silences one more suspect,
// keeping the earlier ones off. The user reports the FIRST STEP where the chop stops ->
// that names the source. Ordered least-to-most disruptive.
static void huntStep(int step) {
    if (step <= 1 && !Audio.isListening()) Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs);
    switch (step) {
        case 0: Serial.println("[HUNT] STEP0 baseline listening (chop should be present)"); break;
        case 1: Serial.println("[HUNT] STEP1 stop the MIC (M5.Mic.end) -> tests mic I2S/DMA activity");
                if (M5.Mic.isRunning()) M5.Mic.end(); break;
        case 2: Serial.println("[HUNT] STEP2 full codec analog power-down 0x0D=0xFC + CSM off -> tests ES8311 analog/amp");
                M5.In_I2C.writeRegister8(0x18,0x32,0x00,100000);
                M5.In_I2C.writeRegister8(0x18,0x13,0x00,100000);
                M5.In_I2C.writeRegister8(0x18,0x12,0x02,100000);
                M5.In_I2C.writeRegister8(0x18,0x0D,0xFC,100000);
                M5.In_I2C.writeRegister8(0x18,0x00,0x00,100000); break;
        case 3: Serial.println("[HUNT] STEP3 blank the screen (display sleep + backlight off) -> tests display SPI/backlight");
                M5.Display.setBrightness(0); M5.Display.sleep(); break;
        case 4: Serial.println("[HUNT] STEP4 WiFi modem-sleep OFF + CPU 240MHz -> tests WiFi power-save beacon ticks");
                WiFi.setSleep(false); setCpuFrequencyMhz(240); break;
        case 5: Serial.println("[HUNT] STEP5 WiFi radio OFF -> tests WiFi RF/power bursts (the classic 'helicopter')");
                WiFi.mode(WIFI_OFF); break;
        default:
                Serial.println("[HUNT] END. If it never went quiet even here, it's a DC-DC/rail whine independent of firmware.");
                Serial.println("[HUNT] reboot to restore normal operation (WiFi/screen were turned off for the test).");
                g_hunt = -1; return;
    }
}

static void consoleLine(String line) {
    line.trim();
    if (line.isEmpty()) return;
    Serial.printf("[CONSOLE] > %s\n", line.c_str());
    if (line.startsWith("/")) { handleCommand(line); return; }
    if (line.startsWith("!")) {
        String c = line.substring(1); c.trim();
        String a; int sp = c.indexOf(' ');
        if (sp >= 0) { a = c.substring(sp + 1); a.trim(); c = c.substring(0, sp); }
        c.toLowerCase();
        if (c == "help") {
            Serial.println("audio: !regs !rd XX !wr XX YY !dac on|off !hp on|off !mute on|off !ana XX");
            Serial.println("drive: !state !wake !sleep !listen !stoplisten !chirp | !hunt / !hunt stop");
            Serial.println("or type '/command' or plain text to talk to her");
        }
        else if (c == "state") Serial.printf("[STATE] awake=%d listening=%d capturing=%d uiState=%d heap=%u micRun=%d spkRun=%d\n",
                                             awake, Audio.isListening(), Audio.capturing(), (int)UI.state(),
                                             (unsigned)ESP.getFreeHeap(), M5.Mic.isRunning(), M5.Speaker.isRunning());
        else if (c == "regs") es8311Dump();
        else if (c == "rd") { uint8_t r = strtoul(a.c_str(), nullptr, 16); Serial.printf("[RD] %02X=%02X\n", r, M5.In_I2C.readRegister8(0x18, r, 100000)); }
        else if (c == "wr") { int s2 = a.indexOf(' '); uint8_t r = strtoul(a.substring(0, s2).c_str(), nullptr, 16); uint8_t v = strtoul(a.substring(s2 + 1).c_str(), nullptr, 16); M5.In_I2C.writeRegister8(0x18, r, v, 100000); Serial.printf("[WR] %02X<=%02X\n", r, v); }
        else if (c == "dac") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x12, on ? 0x00 : 0x02, 100000); Serial.printf("[DAC] %s\n", on ? "up" : "down"); }
        else if (c == "hp") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x13, on ? 0x10 : 0x00, 100000); Serial.printf("[HP] %s\n", on ? "on" : "off"); }
        else if (c == "mute") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x32, on ? 0x00 : 0xBF, 100000); Serial.printf("[DACvol] %s\n", on ? "muted" : "0dB"); }
        else if (c == "ana") { uint8_t v = strtoul(a.c_str(), nullptr, 16); M5.In_I2C.writeRegister8(0x18, 0x0D, v, 100000); Serial.printf("[ANA] 0D<=%02X\n", v); }
        else if (c == "wake") { if (!awake) { UI.showHud(); welcomeProtocol(); } else Serial.println("already awake"); }
        else if (c == "sleep") enterStandby();
        else if (c == "listen") { if (!Audio.isListening()) Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs); Serial.println("[LISTEN] on"); }
        else if (c == "stoplisten") { Audio.stopListening(); Serial.println("[LISTEN] off"); }
        else if (c == "chirp") Audio.chirpWake();
        else if (c == "tone") {   // full speaker I2S re-init + raw tone, report the speaker task state
            if (M5.Mic.isRunning()) M5.Mic.end();
            M5.Speaker.end(); delay(40);
            bool b = M5.Speaker.begin(); M5.Speaker.setVolume(220);
            M5.Speaker.tone(1000, 700, 0, true);
            delay(60);
            Serial.printf("[TONE] begin=%d running=%d playing=%d\n", b, M5.Speaker.isRunning(), M5.Speaker.isPlaying(0));
        }
        else if (c == "pause") { g_listenPaused = true; Audio.stopListening(); Serial.println("[PAUSE] mic OFF (not listening). Is the sound GONE now?"); }
        else if (c == "resume") { g_listenPaused = false; Serial.println("[RESUME] listening will restart. Sound back?"); }
        else if (c == "ui") { g_freezeUI = (a == "off"); Serial.printf("[UI] redraws %s (mic unchanged). Sound %s?\n", g_freezeUI ? "FROZEN" : "live", g_freezeUI ? "gone" : "back"); }
        else if (c == "hunt") { if (a == "stop") { g_hunt = -1; Serial.println("[HUNT] stopped"); } else { g_hunt = 0; g_huntAt = millis() - 4000; Serial.println("[HUNT] starting; tell me the STEP number where the chop stops"); } }
        else Serial.println("[CONSOLE] unknown ! command (try !help)");
        return;
    }
    String lower = line; lower.toLowerCase();
    if (!awake) {
        if (lower.indexOf("wake up") >= 0) { UI.showHud(); welcomeProtocol(); }
        else Serial.println("[CONSOLE] she's asleep - send 'wake up diana' first");
        return;
    }
    UI.log(line, 'u'); appendChatLog('u', line); g_lastUserMsg = line;
    runTurn(line, nullptr, 0);
}

static void serviceSerialConsole() {
    static String buf;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') { consoleLine(buf); buf = ""; }
        else if (buf.length() < 220) buf += ch;
    }
    if (g_hunt >= 0 && millis() - g_huntAt > 4000) { g_huntAt = millis(); huntStep(g_hunt++); }
}

void loop() {
    M5Cardputer.update();
    serviceSerialConsole();

    // Pixel music player (opened by /music or the music_play voice tool)
    if (g_musicReq && !Music.isOpen()) {
        g_musicReq = false;
        Music.open(0);
        if (g_musicTrack.length()) Music.playByName(g_musicTrack);
        g_musicTrack = "";
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
        } else {
            if (UI.state() == DianaState::SETUP) { /* portal opened from menu */ }
            else if (awake) UI.showHud();
            else UI.showStandby();
        }
        delay(5);
        return;
    }

    if (UI.state() == DianaState::SETUP) {
        static uint32_t lastClients = 0;
        static int shownClients = -1;
        if (!Setup.loop()) {                       // saved -> reboot
            delay(300);
            ESP.restart();
        }
        if (millis() - lastClients > 1000) {
            lastClients = millis();
            int c = Setup.clients();
            if (c != shownClients) { shownClients = c; UI.showSetup(SETUP_AP_SSID, Setup.url(), c); }
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

    handleKeys();
    serviceTimers();
    // NOTE: continuous "hands-free" listening and spoken-wake are intentionally OFF.
    // Holding the ES8311 mic on continuously coupled a rhythmic chop into the always-on
    // amp, so input is push-to-talk only: hold TAB to speak. (Voice tools/console can still
    // drive listening explicitly for debugging.)

    if (millis() - lastStatusMs > 2000) {
        lastStatusMs = millis();
        refreshStatusBar();
        if (awake && Net.isConnected() == false && Config.hasWifi()) {
            static uint32_t lastRetry = 0;
            if (millis() - lastRetry > 30000) { lastRetry = millis(); Net.connect(6000, nullptr); }
        }
    }
    if (!g_freezeUI) UI.tick();
    delay(5);
}
