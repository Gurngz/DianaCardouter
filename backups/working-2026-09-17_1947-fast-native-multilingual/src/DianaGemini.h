// Gemini REST client: chat with function calling (text or microphone audio),
// text-to-speech streamed straight to the SD card, and Google-grounded search.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <vector>
#include <functional>
#include "DianaHttp.h"

struct GeminiCall {
    String name;
    String id;
    String argsJson;      // JSON object text
};

struct GeminiResult {
    bool   ok = false;
    int    status = 0;
    String error;
    String text;          // all text parts joined
    String finishReason;
    std::vector<GeminiCall> calls;
};

struct FunctionResponse {
    String name;
    String id;
    String responseJson;  // JSON object text, e.g. {"result":"..."}
};

class DianaGemini {
public:
    void   setSystemPrompt(const String& s) { _systemPrompt = s; }
    void   clearHistory() { _history.clear(); }
    size_t historySize() const { return _history.size(); }

    // One model call. userText is sent as a text part; if audioPath is given the
    // PCM file is streamed as an inlineData part (audio/L16;rate=16000).
    bool generate(const String& userText, const char* audioPath, size_t audioBytes, GeminiResult& out);

    // Send the results of the tool calls from the previous turn.
    bool sendFunctionResponses(const std::vector<FunctionResponse>& responses, GeminiResult& out);

    // Add a text user turn to history without calling the model (used for voice transcripts).
    void recordUserTurn(const String& text);
    // Seed one past exchange (user + Diana) as plain text — used to reload saved history on boot.
    void seedTurn(const String& userText, const String& modelText);
    // Replace the most recent plain user turn (the voice placeholder) with its transcript.
    void replaceLastUserTurn(const String& text);
    // Drop the most recent user turn (and anything after it) — for ignored wake-word misses.
    void rollbackLastUserTurn();

    // Text-to-speech -> raw 16-bit 24 kHz PCM written to outPath. Returns bytes written.
    bool tts(const String& text, const char* outPath, size_t& bytesOut, String& err);

    // Streaming TTS: audio chunks (16-bit 24 kHz PCM) are delivered to onPcm as they
    // arrive over SSE, so playback can start ~1-2 s in instead of after the whole clip.
    // tick() is polled between chunks; return false to abort. Returns true if any audio played.
    bool ttsStream(const String& text, const char* model, const String& stylePrefix,
                   std::function<void(const uint8_t*, size_t)> onPcm,
                   std::function<bool()> tick, String& err);
    int lastTtsStatus = 0;   // HTTP status of the last ttsStream (429 = quota)

    // Cheap transcription-only call (no tools/history) — used for the spoken wake phrase.
    bool transcribe(const char* audioPath, size_t audioBytes, String& out, String& err);

    // Google-search-grounded answer (separate call; the free tier may refuse with 429).
    bool groundedSearch(const String& query, String& answer, String& err);

    void disconnect() { _http.close(); }

    static String jsonEscape(const String& s);

private:
    HttpsStream _http;
    String _systemPrompt;
    std::vector<String> _history;     // raw "content" objects, oldest first
    String _pendingUserTurn;          // text form of the current user turn (for history)

    bool   callModel(const String& head, const char* audioPath, size_t audioBytes, const String& tail,
                     GeminiResult& out);
    bool   sendRequest(const char* model, const String& head, const char* audioPath, size_t audioBytes,
                       const String& tail, JsonDocument& doc, int& status, String& err);
    String historyJson() const;
    void   pushHistory(const String& contentJson);
    void   trimHistory();
    void   dropTrailingToolCall();
    String requestTail() const;
};

extern DianaGemini Gemini;
