#include "DianaConfig.h"
#include "config.h"
#include <ArduinoJson.h>
#include <Preferences.h>
#include <SD.h>

DianaConfig Config;

static const char* NVS_NS = "diana";

const char* DianaConfig::chatModelOrDefault() const { return chatModel.length() ? chatModel.c_str() : DEFAULT_CHAT_MODEL; }
const char* DianaConfig::ttsModelOrDefault()  const { return ttsModel.length()  ? ttsModel.c_str()  : DEFAULT_TTS_MODEL; }
const char* DianaConfig::ttsVoiceOrDefault()  const { return ttsVoice.length()  ? ttsVoice.c_str()  : DEFAULT_TTS_VOICE; }
const char* DianaConfig::ttsStyleOrDefault()  const { return ttsStyle.length()  ? ttsStyle.c_str()  : DEFAULT_TTS_STYLE; }

static bool isPlaceholderKey(const String& k) {
    return k.length() < 12 || k.startsWith("PASTE") || k.startsWith("YOUR");
}

void DianaConfig::addApiKey(const String& kIn) {
    String k = kIn; k.trim();
    if (isPlaceholderKey(k)) return;
    for (auto& e : apiKeys) if (e == k) return;   // dedupe
    apiKeys.push_back(k);
    if (apiKeys.size() > 8) apiKeys.erase(apiKeys.begin());
}

bool DianaConfig::removeApiKey(int index) {
    if (index < 0 || index >= (int)apiKeys.size()) return false;
    apiKeys.erase(apiKeys.begin() + index);
    // keep the active index pointing at a valid key
    if (apiKeyIndex >= (int)apiKeys.size()) apiKeyIndex = apiKeys.empty() ? 0 : (int)apiKeys.size() - 1;
    else if (apiKeyIndex > index) apiKeyIndex--;
    syncActiveKey();
    return true;
}

void DianaConfig::syncActiveKey() {
    if (apiKeys.empty()) { apiKey = ""; return; }
    if (apiKeyIndex < 0 || apiKeyIndex >= (int)apiKeys.size()) apiKeyIndex = 0;
    apiKey = apiKeys[apiKeyIndex];
}

bool DianaConfig::rotateApiKey() {
    if (apiKeys.size() <= 1) return false;
    apiKeyIndex = (apiKeyIndex + 1) % apiKeys.size();
    syncActiveKey();
    // persist just the index so a reboot resumes on the same key
    Preferences p;
    if (p.begin(NVS_NS, false)) { p.putInt("keyidx", apiKeyIndex); p.end(); }
    return true;
}

void DianaConfig::setWifi(const String& ssid, const String& pass) {
    for (auto& w : wifi) {
        if (w.ssid == ssid) { w.pass = pass; return; }
    }
    wifi.insert(wifi.begin(), WifiCred{ssid, pass});
    if (wifi.size() > 5) wifi.pop_back();
}

bool DianaConfig::load(bool sdAvailable) {
    loaded = false;
    fromSd = false;
    apiKeys.clear();
    bool hadConfig = false;
    if (sdAvailable && loadFromSd()) {
        fromSd = true; hadConfig = true;
    } else {
        hadConfig = loadFromNvs();
    }
    // Union any keys stored in NVS (baked-in keys augment whatever the SD card has).
    mergeNvsKeys();
    // One-off migrations, applied once per config (gated so /mic and silence_ms edits survive a reboot).
    if (!hadConfig) configVersion = CONFIG_VERSION;   // fresh device: defaults are already current
    if (configVersion < 2) {
        if (ttsModel == "gemini-2.5-flash-preview-tts") ttsModel = "";   // free-tier-throttled -> streaming 3.1
        if (silenceMs > 900) silenceMs = 700;          // snappier end-of-speech from older configs
        if (micGain > 40) micGain = DEFAULT_MIC_GAIN;  // old configs baked 48 (too hot -> noise read as speech)
        configVersion = CONFIG_VERSION;
    }
    syncActiveKey();
    saveToNvs();                               // persist the merged key list + settings
    loaded = apiKey.length() > 10;
    return loaded;
}

bool DianaConfig::save(bool sdAvailable) {
    bool ok = saveToNvs();
    if (sdAvailable) ok = saveToSd() && ok;
    loaded = apiKey.length() > 10;
    return ok;
}

bool DianaConfig::loadFromSd() {
    File f = SD.open(CONFIG_PATH, FILE_READ);
    if (!f) return false;
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, f);
    f.close();
    if (err) {
        Serial.printf("[CFG] config.json parse error: %s\n", err.c_str());
        return false;
    }
    wifi.clear();
    if (doc["wifi"].is<JsonArray>()) {
        for (JsonObject w : doc["wifi"].as<JsonArray>()) {
            String s = w["ssid"] | "";
            if (s.startsWith("YOUR-")) continue;          // template placeholder
            if (s.length()) wifi.push_back(WifiCred{s, String(w["pass"] | "")});
        }
    } else if (doc["wifi_ssid"].is<const char*>()) {
        wifi.push_back(WifiCred{String(doc["wifi_ssid"] | ""), String(doc["wifi_pass"] | "")});
    }
    // API keys: accept a list ("gemini_api_keys": [...]) and/or a single "gemini_api_key".
    if (doc["gemini_api_keys"].is<JsonArray>()) {
        for (JsonVariant v : doc["gemini_api_keys"].as<JsonArray>()) addApiKey(String(v.as<const char*>() ? v.as<const char*>() : ""));
    }
    addApiKey(String(doc["gemini_api_key"] | ""));
    apiKeyIndex  = doc["api_key_index"]  | 0;
    chatModel    = doc["chat_model"]     | "";
    ttsModel     = doc["tts_model"]      | "";
    ttsVoice     = doc["tts_voice"]      | "";
    ttsStyle     = doc["tts_style"]      | "";
    userName     = doc["user_name"]      | "";
    tz           = doc["timezone"]       | "";
    font         = doc["font"]           | DEFAULT_FONT;
    thinkingLevel = doc["thinking_level"] | DEFAULT_THINKING_LEVEL;
    voiceEnabled = doc["voice_enabled"]  | DEFAULT_VOICE_ENABLED;
    bootMusic    = doc["boot_music"]     | DEFAULT_BOOT_MUSIC;
    autoWake     = doc["auto_wake"]      | DEFAULT_AUTO_WAKE;
    autoStop     = doc["auto_stop_recording"] | DEFAULT_AUTO_STOP;
    handsFree    = doc["hands_free"]     | DEFAULT_HANDS_FREE;
    voiceWake    = doc["voice_wake"]     | DEFAULT_VOICE_WAKE;
    wakeWord     = doc["wake_word"]      | DEFAULT_WAKE_WORD;
    micGain      = doc["mic_gain"]       | DEFAULT_MIC_GAIN;
    volume       = doc["volume"]         | DEFAULT_VOLUME;
    brightness   = doc["brightness"]     | DEFAULT_BRIGHTNESS;
    vadThreshold = doc["vad_threshold"]  | DEFAULT_VAD_THRESHOLD;
    silenceMs    = doc["silence_ms"]     | DEFAULT_SILENCE_MS;
    idleSleepSec = doc["idle_sleep_sec"]  | DEFAULT_IDLE_SLEEP_SEC;
    configVersion = doc["config_version"] | 0;
    return true;
}

bool DianaConfig::saveToSd() {
    JsonDocument doc;
    JsonArray arr = doc["wifi"].to<JsonArray>();
    for (auto& w : wifi) {
        JsonObject o = arr.add<JsonObject>();
        o["ssid"] = w.ssid;
        o["pass"] = w.pass;
    }
    JsonArray keys = doc["gemini_api_keys"].to<JsonArray>();
    for (auto& k : apiKeys) keys.add(k);
    doc["gemini_api_key"] = apiKeys.empty() ? "" : apiKeys[0];   // back-compat
    doc["api_key_index"]  = apiKeyIndex;
    doc["chat_model"]     = chatModel;
    doc["tts_model"]      = ttsModel;
    doc["tts_voice"]      = ttsVoice;
    doc["tts_style"]      = ttsStyle;
    doc["user_name"]      = userName;
    doc["timezone"]       = tz;
    doc["font"]           = font;
    doc["thinking_level"] = thinkingLevel;
    doc["voice_enabled"]  = voiceEnabled;
    doc["boot_music"]     = bootMusic;
    doc["auto_wake"]      = autoWake;
    doc["auto_stop_recording"] = autoStop;
    doc["hands_free"]     = handsFree;
    doc["voice_wake"]     = voiceWake;
    doc["wake_word"]      = wakeWord;
    doc["mic_gain"]       = micGain;
    doc["volume"]         = volume;
    doc["brightness"]     = brightness;
    doc["vad_threshold"]  = vadThreshold;
    doc["silence_ms"]     = silenceMs;
    doc["idle_sleep_sec"] = idleSleepSec;
    doc["config_version"] = configVersion;
    if (!SD.exists(DIANA_DIR)) SD.mkdir(DIANA_DIR);
    File f = SD.open(CONFIG_PATH, FILE_WRITE);
    if (!f) return false;
    serializeJsonPretty(doc, f);
    f.close();
    return true;
}

bool DianaConfig::loadFromNvs() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return false;
    wifi.clear();
    for (int i = 0; i < 5; ++i) {
        String s = p.getString((String("ssid") + i).c_str(), "");
        if (s.length()) wifi.push_back(WifiCred{s, p.getString((String("pass") + i).c_str(), "")});
    }
    int nk = p.getInt("keycount", 0);
    if (nk > 0) {
        for (int i = 0; i < nk && i < 8; ++i) addApiKey(p.getString((String("key") + i).c_str(), ""));
    } else {
        addApiKey(p.getString("apikey", ""));    // legacy single key
    }
    apiKeyIndex  = p.getInt("keyidx", 0);
    chatModel    = p.getString("chatmodel", "");
    ttsModel     = p.getString("ttsmodel", "");
    ttsVoice     = p.getString("ttsvoice", "");
    ttsStyle     = p.getString("ttsstyle", "");
    userName     = p.getString("username", "");
    tz           = p.getString("tz", "");
    font         = p.getString("font", DEFAULT_FONT);
    thinkingLevel = p.getString("think", DEFAULT_THINKING_LEVEL);
    voiceEnabled = p.getBool("voice", DEFAULT_VOICE_ENABLED);
    bootMusic    = p.getBool("bootmusic", DEFAULT_BOOT_MUSIC);
    autoWake     = p.getBool("autowake", DEFAULT_AUTO_WAKE);
    autoStop     = p.getBool("autostop", DEFAULT_AUTO_STOP);
    handsFree    = p.getBool("handsfree", DEFAULT_HANDS_FREE);
    voiceWake    = p.getBool("vwake", DEFAULT_VOICE_WAKE);
    wakeWord     = p.getString("wakeword", DEFAULT_WAKE_WORD);
    micGain      = p.getInt("micgain", DEFAULT_MIC_GAIN);
    volume       = p.getInt("volume", DEFAULT_VOLUME);
    brightness   = p.getInt("bright", DEFAULT_BRIGHTNESS);
    vadThreshold = p.getInt("vad", DEFAULT_VAD_THRESHOLD);
    silenceMs    = p.getInt("silence", DEFAULT_SILENCE_MS);
    idleSleepSec = p.getInt("idlesleep", DEFAULT_IDLE_SLEEP_SEC);
    configVersion = p.getInt("cfgver", 0);
    p.end();
    return !apiKeys.empty() || !wifi.empty();
}

// Union any keys stored in the NVS "key<n>" list into apiKeys (so baked-in keys persist
// even when the SD config only carries one). Called after the primary load.
void DianaConfig::mergeNvsKeys() {
    Preferences p;
    if (!p.begin(NVS_NS, true)) return;
    int nk = p.getInt("keycount", 0);
    for (int i = 0; i < nk && i < 8; ++i) addApiKey(p.getString((String("key") + i).c_str(), ""));
    p.end();
}

bool DianaConfig::saveToNvs() {
    Preferences p;
    if (!p.begin(NVS_NS, false)) return false;
    for (int i = 0; i < 5; ++i) {
        String ks = String("ssid") + i, kp = String("pass") + i;
        if (i < (int)wifi.size()) {
            p.putString(ks.c_str(), wifi[i].ssid);
            p.putString(kp.c_str(), wifi[i].pass);
        } else {
            p.remove(ks.c_str());
            p.remove(kp.c_str());
        }
    }
    // key list
    p.putInt("keycount", (int)apiKeys.size());
    p.putInt("keyidx", apiKeyIndex);
    for (int i = 0; i < 8; ++i) {
        String kk = String("key") + i;
        if (i < (int)apiKeys.size()) p.putString(kk.c_str(), apiKeys[i]);
        else p.remove(kk.c_str());
    }
    p.putString("apikey", apiKeys.empty() ? "" : apiKeys[0]);   // legacy mirror
    p.putString("chatmodel", chatModel);
    p.putString("ttsmodel", ttsModel);
    p.putString("ttsvoice", ttsVoice);
    p.putString("ttsstyle", ttsStyle);
    p.putString("username", userName);
    p.putString("tz", tz);
    p.putString("font", font);
    p.putString("think", thinkingLevel);
    p.putBool("voice", voiceEnabled);
    p.putBool("bootmusic", bootMusic);
    p.putBool("autowake", autoWake);
    p.putBool("autostop", autoStop);
    p.putBool("handsfree", handsFree);
    p.putBool("vwake", voiceWake);
    p.putString("wakeword", wakeWord);
    p.putInt("micgain", micGain);
    p.putInt("volume", volume);
    p.putInt("bright", brightness);
    p.putInt("vad", vadThreshold);
    p.putInt("silence", silenceMs);
    p.putInt("idlesleep", idleSleepSec);
    p.putInt("cfgver", configVersion);
    p.end();
    return true;
}
