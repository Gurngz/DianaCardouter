// Configuration: /diana/config.json on the SD card, mirrored to NVS so the
// device also works after the card is removed.
#pragma once
#include <Arduino.h>
#include <vector>
#include "config.h"

struct WifiCred {
    String ssid;
    String pass;
};

class DianaConfig {
public:
    std::vector<WifiCred> wifi;
    std::vector<String> apiKeys;      // one or more Gemini keys; rotated on quota (429)
    int    apiKeyIndex = 0;
    String apiKey;                    // mirror of apiKeys[apiKeyIndex] (the active key)
    String chatModel   = "";      // empty -> DEFAULT_CHAT_MODEL
    String ttsModel    = "";      // empty -> DEFAULT_TTS_MODEL
    String ttsVoice    = "";      // empty -> DEFAULT_TTS_VOICE
    String ttsStyle    = "";      // empty -> DEFAULT_TTS_STYLE
    String userName    = "";
    String tz          = "";      // POSIX TZ string, e.g. "JST-9" (empty -> from ip-api)
    String font        = "ascii"; // "ascii" | "jp"
    String thinkingLevel = "minimal"; // Gemini 3.x thinkingLevel; "" = model default
    bool   voiceEnabled = true;
    bool   bootMusic    = true;
    bool   autoWake     = false;  // skip standby screen after boot
    bool   autoStop     = true;   // stop recording on silence
    bool   handsFree    = true;   // continuous listening; say the wake word instead of pressing TAB
    bool   voiceWake    = true;   // while asleep, listen for the spoken wake phrase (uses cloud transcription)
    String wakeWord     = "diana";
    int    micGain      = DEFAULT_MIC_GAIN;
    int    volume       = 200;    // 0-255
    int    brightness   = 160;    // 0-255
    int    vadThreshold = 550;    // RMS level that counts as speech
    int    silenceMs    = 1200;   // silence after speech that ends a recording
    int    idleSleepSec = 60;     // auto-return to standby after this many idle seconds (0 = never)

    bool loaded = false;          // true when apiKey present
    bool fromSd = false;

    bool load(bool sdAvailable);
    bool save(bool sdAvailable);
    bool hasWifi() const { return !wifi.empty(); }
    void setWifi(const String& ssid, const String& pass);

    void addApiKey(const String& k);       // append if valid and not a duplicate
    bool removeApiKey(int index);          // drop key at index; keeps apiKeyIndex valid. false if bad index
    int  apiKeyCount() const { return (int)apiKeys.size(); }
    bool rotateApiKey();                    // advance to the next key on quota; false if only one
    void syncActiveKey();                   // set apiKey = apiKeys[apiKeyIndex]

    const char* chatModelOrDefault() const;
    const char* ttsModelOrDefault() const;
    const char* ttsVoiceOrDefault() const;
    const char* ttsStyleOrDefault() const;

private:
    bool loadFromSd();
    bool loadFromNvs();
    bool saveToSd();
    bool saveToNvs();
    void mergeNvsKeys();
};

extern DianaConfig Config;
