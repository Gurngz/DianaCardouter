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
    String font        = DEFAULT_FONT;
    String thinkingLevel = DEFAULT_THINKING_LEVEL; // Gemini 3.x thinkingLevel; "" = model default
    bool   voiceEnabled = DEFAULT_VOICE_ENABLED;
    bool   bootMusic    = DEFAULT_BOOT_MUSIC;
    bool   autoWake     = DEFAULT_AUTO_WAKE;   // stored for config compatibility; boot always wakes (PTT mode)
    bool   autoStop     = DEFAULT_AUTO_STOP;   // stop recording on silence
    bool   handsFree    = DEFAULT_HANDS_FREE;  // continuous listening while awake
    bool   voiceWake    = DEFAULT_VOICE_WAKE;  // stored for config compatibility; spoken wake from standby is not implemented
    String wakeWord     = DEFAULT_WAKE_WORD;
    int    micGain      = DEFAULT_MIC_GAIN;
    int    volume       = DEFAULT_VOLUME;      // 0-255
    int    brightness   = DEFAULT_BRIGHTNESS;  // 0-255
    int    vadThreshold = DEFAULT_VAD_THRESHOLD; // RMS level that counts as speech
    int    silenceMs    = DEFAULT_SILENCE_MS;  // silence after speech that ends a recording
    int    idleSleepSec = DEFAULT_IDLE_SLEEP_SEC; // stored for config compatibility; auto-sleep is not implemented
    int    configVersion = 0;     // last migration applied (see CONFIG_VERSION)

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
