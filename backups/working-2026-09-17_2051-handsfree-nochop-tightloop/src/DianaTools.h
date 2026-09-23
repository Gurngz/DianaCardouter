// Gemini function-calling tools available in the Cardputer body.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <functional>

struct ToolHooks {
    std::function<String(const String& query)> webSearch;
    std::function<void(bool muted)> setMuted;
    std::function<void()> requestShutdown;
    std::function<String(int seconds, const String& label)> addTimer;
    std::function<String()> deviceStatus;
    std::function<String(const String& setting, const String& value)> deviceSetting;
    std::function<String(const String& ssid, const String& password)> connectWifi;
    std::function<String()> scanWifi;
    std::function<String()> whereAmI;
    std::function<void()> openSettings;
    std::function<String(const String& track)> musicPlay;
};

namespace DianaTools {
    // JSON text for the "tools" array of a generateContent request
    extern const char DECLARATIONS[] PROGMEM;
    // Executes a tool; returns the JSON object text to send back as functionResponse.response
    String execute(const String& name, JsonObjectConst args, ToolHooks& hooks);
    bool   isNetworkTool(const String& name);
}
