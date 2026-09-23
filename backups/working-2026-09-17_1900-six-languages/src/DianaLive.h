// Gemini Live API (native-audio) client — the same voice pipeline PC Diana uses
// (models/gemini-2.5-flash-native-audio-preview). It rides the Live API's generous
// session quota instead of the 10-requests/day cap on the REST TTS preview models,
// and speaks many languages natively.
//
// We use it as a high-quota, expressive VOICE engine: the reply text (already produced
// by the REST chat + tools) is sent into a Live session with a "speak this verbatim"
// instruction, and the 24 kHz PCM that streams back is played on the fly. A raw,
// hand-rolled WebSocket-over-TLS keeps RAM in check on the no-PSRAM ESP32-S3.
#pragma once
#include <Arduino.h>
#include <functional>

class DianaLive {
public:
    // Speak `text` through the Live native-audio model. `voice` is a prebuilt voice name
    // (e.g. "Leda", "Charon"). `style` is an optional tone hint ("warmly", "excited"...).
    // Streams 24 kHz mono PCM to onPcm as it arrives; `tick` is polled and may return false
    // to abort. Returns true if any audio was produced. On failure, err is set and
    // lastStatus holds the WS upgrade status (101 ok) or a negative transport code.
    bool speak(const String& text, const String& voice, const String& style,
               const char* apiKey,
               std::function<void(const uint8_t*, size_t)> onPcm,
               std::function<bool()> tick, String& err);

    int lastStatus = 0;
};

extern DianaLive GeminiLive;
