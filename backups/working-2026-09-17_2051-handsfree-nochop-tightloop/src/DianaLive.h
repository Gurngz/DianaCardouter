// Gemini Live API (native-audio) client — the same voice pipeline PC Diana uses
// (models/gemini-2.5-flash-native-audio-preview). Rides the Live API's session quota, not
// the 10-requests/day cap on the REST TTS preview models, and speaks many languages natively.
//
// It keeps ONE WebSocket session open across replies: the TLS + WS handshake + setup happen
// once (slow), then every reply just sends the text and streams the 24 kHz PCM back (fast,
// ~1 s). A hand-rolled WebSocket-over-TLS keeps RAM in check on the no-PSRAM ESP32-S3.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>
#include <functional>

class DianaLive {
public:
    // Speak `text` through the Live native-audio model, reusing an open session when possible.
    // `voice` is a prebuilt voice name (e.g. "Leda"); `style` an optional tone hint. Streams
    // 24 kHz mono PCM to onPcm; `tick` is polled and may return false to abort. Returns true if
    // any audio was produced. On failure err is set and the session is closed so the next call reopens.
    bool speak(const String& text, const String& voice, const String& style, const String& langCode,
               const char* apiKey,
               std::function<void(const uint8_t*, size_t)> onPcm,
               std::function<bool()> tick, String& err);

    void end();          // close the persistent session (frees the TLS socket)
    void keepAlive();    // call from the main loop: pings the open session so it doesn't idle-drop
    // True if the next speak() with these params will reuse the open session (no reconnect).
    bool willReuse(const String& voice, const String& style, const String& langCode, const char* apiKey) {
        return usable(voice, style, langCode, apiKey);
    }
    int lastStatus = 0;

private:
    WiFiClientSecure _ws;
    bool   _ready = false;             // session open + setupComplete received
    String _key, _voice, _style, _lang;   // params the current session was opened with
    uint32_t _lastPing = 0;               // keepAlive throttle

    bool openSession(const String& voice, const String& style, const String& langCode, const char* apiKey, String& err);
    void closeSession();
    bool usable(const String& voice, const String& style, const String& langCode, const char* apiKey);
};

extern DianaLive GeminiLive;
