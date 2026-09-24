#include "DianaGemini.h"
#include "DianaJson.h"
#include "DianaTune.h"
#include "DianaConfig.h"
#include "DianaTools.h"
#include "Base64Stream.h"
#include "config.h"
#include <SD.h>

DianaGemini Gemini;

// Parse a Gemini error body: fill msg + (for 429) any retryDelay, and log it clearly to
// serial with a rate-limit note. Returns true if it was a rate-limit (429/RESOURCE_EXHAUSTED).
static bool logGeminiError(const char* where, const char* model, int status, const String& body) {
    JsonDocument ed;
    String msg, statusStr, retry;
    if (!deserializeJson(ed, body)) {
        msg = (const char*)(ed["error"]["message"] | "");
        statusStr = (const char*)(ed["error"]["status"] | "");
        for (JsonObject d : ed["error"]["details"].as<JsonArray>()) {
            if (d["retryDelay"].is<const char*>()) retry = (const char*)d["retryDelay"];
            const char* qm = d["violations"][0]["quotaMetric"] | "";
            if (*qm && msg.indexOf("quota") < 0) msg += String(" [") + qm + "]";
        }
    }
    if (msg.isEmpty()) msg = body.substring(0, 160);
    bool rate = (status == 429) || statusStr == "RESOURCE_EXHAUSTED";
    Serial.printf("[GEMINI ERR] %s model=%s HTTP %d %s%s\n  %s\n", where, model ? model : "?",
                  status, statusStr.c_str(), rate ? "  <<< RATE LIMIT / QUOTA" : "", msg.c_str());
    if (retry.length()) Serial.printf("  retry after: %s\n", retry.c_str());
    return rate;
}

String DianaGemini::jsonEscape(const String& s) { return jsonEscaped(s); }

// ───────────────────────────── history ───────────────────────────────────
void DianaGemini::pushHistory(const String& contentJson) {
    _history.push_back(contentJson);
    trimHistory();
}

void DianaGemini::trimHistory() {
    auto totalChars = [&]() {
        size_t n = 0;
        for (auto& h : _history) n += h.length();
        return n;
    };
    // never drop the newest entry: a large tool-call turn must survive so its functionResponse has a parent
    while (_history.size() > 1 && ((int)_history.size() > Tune.historyMaxTurns * 2 || totalChars() > (size_t)Tune.historyMaxChars)) {
        _history.erase(_history.begin());
    }
    // the conversation must start with a plain user turn (not a tool result, not a model turn)
    while (!_history.empty() &&
           (_history.front().indexOf("\"role\":\"user\"") < 0 || _history.front().indexOf("functionResponse") >= 0)) {
        _history.erase(_history.begin());
    }
}

String DianaGemini::historyJson() const {
    String s;
    size_t n = 0;
    for (auto& h : _history) n += h.length() + 1;
    s.reserve(n);
    for (size_t i = 0; i < _history.size(); ++i) {
        if (i) s += ',';
        s += _history[i];
    }
    return s;
}

void DianaGemini::recordUserTurn(const String& text) {
    pushHistory("{\"role\":\"user\",\"parts\":[{\"text\":\"" + jsonEscape(text) + "\"}]}");
}

void DianaGemini::seedTurn(const String& userText, const String& modelText) {
    if (userText.length())
        pushHistory("{\"role\":\"user\",\"parts\":[{\"text\":\"" + jsonEscape(userText) + "\"}]}");
    if (modelText.length())
        pushHistory("{\"role\":\"model\",\"parts\":[{\"text\":\"" + jsonEscape(modelText) + "\"}]}");
}

void DianaGemini::rollbackLastUserTurn() {
    // Drop everything from the most recent plain user turn to the end of history,
    // so an unaddressed hands-free utterance leaves no trace in the context.
    for (int i = (int)_history.size() - 1; i >= 0; --i) {
        if (_history[i].indexOf("\"role\":\"user\"") >= 0 && _history[i].indexOf("functionResponse") < 0) {
            _history.erase(_history.begin() + i, _history.end());
            return;
        }
    }
}

void DianaGemini::replaceLastUserTurn(const String& text) {
    // walk back to the most recent plain user turn (the "[voice message]" placeholder)
    for (int i = (int)_history.size() - 1; i >= 0; --i) {
        if (_history[i].indexOf("\"role\":\"user\"") >= 0 && _history[i].indexOf("functionResponse") < 0) {
            _history[i] = "{\"role\":\"user\",\"parts\":[{\"text\":\"" + jsonEscape(text) + "\"}]}";
            return;
        }
    }
    recordUserTurn(text);
}

String DianaGemini::requestTail() const {
    String tail = "],\"tools\":";
    tail += FPSTR(DianaTools::DECLARATIONS);
    tail += ",\"generationConfig\":{\"maxOutputTokens\":" + String(Tune.replyMaxTokens) + ",\"temperature\":0.9";
    if (Config.thinkingLevel.length()) tail += ",\"thinkingConfig\":{\"thinkingLevel\":\"" + jsonEscape(Config.thinkingLevel) + "\"}";
    tail += "}}";
    return tail;
}

// ───────────────────────────── transport ─────────────────────────────────
bool DianaGemini::sendRequest(const char* model, const String& head, const char* audioPath, size_t audioBytes,
                              const String& tail, JsonDocument& doc, int& status, String& err) {
    size_t b64Len = audioPath ? Base64Encoder::encodedLength(audioBytes) : 0;
    size_t contentLength = head.length() + b64Len + tail.length();
    String path = String("/v1beta/models/") + model + ":generateContent";

    int maxAttempts = 2 + Config.apiKeyCount();     // extra tries so we can rotate keys on 429
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        if (attempt) { _http.close(); delay(200); }
        if (!_http.begin(GEMINI_HOST, "POST", path, contentLength, Config.apiKey.c_str())) {
            err = _http.lastError;
            Serial.printf("[GEMINI ERR] TLS connect failed (attempt %d/%d): %s\n", attempt + 1, maxAttempts, err.c_str());
            continue;
        }
        bool wroteOk = _http.writeStr(head) == head.length();
        if (wroteOk && audioPath) {
            File f = SD.open(audioPath, FILE_READ);
            if (!f) { err = "audio file missing"; _http.close(); return false; }
            Base64Encoder enc;
            uint8_t in[768];
            char out[1024 + 8];
            size_t left = audioBytes;
            while (left > 0 && wroteOk) {
                size_t want = left < sizeof(in) ? left : sizeof(in);
                size_t r = f.read(in, want);
                if (r == 0) { // file shorter than expected: pad with silence
                    memset(in, 0, want);
                    r = want;
                }
                size_t n = enc.feed(in, r, out);
                wroteOk = _http.write((const uint8_t*)out, n) == n;
                left -= r;
            }
            size_t n = enc.finish(out);
            if (wroteOk && n) wroteOk = _http.write((const uint8_t*)out, n) == n;
            f.close();
        }
        if (wroteOk) wroteOk = _http.writeStr(tail) == tail.length();
        if (!wroteOk) { err = "request write failed"; continue; }

        status = _http.finishRequest(45000);
        if (status < 0) {
            err = _http.lastError;
            Serial.printf("[GEMINI ERR] no response (attempt %d/%d): %s\n", attempt + 1, maxAttempts, err.c_str());
            continue;
        }

        if (status != 200) {
            String body = _http.readBodyToString(4000);
            bool rate = logGeminiError("chat/generateContent", model, status, body);
            JsonDocument ed;
            const char* m = (!deserializeJson(ed, body)) ? (ed["error"]["message"] | "") : "";
            err = rate ? (String("rate limit / quota (HTTP ") + status + ")") : (String("HTTP ") + status);
            if (m && *m) err += String(": ") + m;
            // Quota on this key? rotate to the next one and retry.
            if (rate && Config.rotateApiKey()) {
                Serial.printf("[KEY] rate-limited -> rotating to key #%d of %d\n", Config.apiKeyIndex, Config.apiKeyCount());
                _http.close();
                continue;
            }
            if (rate) Serial.println("[KEY] all keys rate-limited (or single key) - giving up this call");
            return false;
        }
        DeserializationError de = deserializeJson(doc, _http);
        _http.drain();
        if (de) {
            err = String("JSON: ") + de.c_str();
            _http.close();
            return false;
        }
        return true;
    }
    if (err.isEmpty()) err = "connection failed";
    return false;
}

bool DianaGemini::callModel(const String& head, const char* audioPath, size_t audioBytes, const String& tail,
                            GeminiResult& out) {
    out = GeminiResult();
    JsonDocument doc;
    if (!sendRequest(Config.chatModelOrDefault(), head, audioPath, audioBytes, tail, doc, out.status, out.error)) {
        _pendingUserTurn = "";          // never let a failed turn leak into history
        return false;
    }
    JsonObject cand = doc["candidates"][0];
    if (cand.isNull()) {
        const char* block = doc["promptFeedback"]["blockReason"] | "";
        out.error = *block ? String("blocked: ") + block : String("empty response");
        return false;
    }
    out.finishReason = cand["finishReason"] | "";
    JsonObject content = cand["content"];
    if (content.isNull()) {
        out.error = "no content (" + out.finishReason + ")";
        return false;
    }
    bool hasCall = false;
    for (JsonObject part : content["parts"].as<JsonArray>()) {
        if (part["text"].is<const char*>()) {
            if (out.text.length()) out.text += "\n";
            out.text += (const char*)part["text"];
        }
        if (part["functionCall"].is<JsonObject>()) {
            hasCall = true;
            GeminiCall c;
            c.name = part["functionCall"]["name"] | "";
            c.id = part["functionCall"]["id"] | "";
            serializeJson(part["functionCall"]["args"], c.argsJson);
            out.calls.push_back(c);
        }
    }
    // Keep the model turn verbatim when it carries tool calls (thoughtSignature is mandatory
    // for the follow-up); otherwise keep just the text to save history space.
    if (!content["role"].is<const char*>()) content["role"] = "model";
    String modelJson;
    if (hasCall) {
        serializeJson(content, modelJson);
    } else {
        modelJson = "{\"role\":\"model\",\"parts\":[{\"text\":\"" + jsonEscape(out.text) + "\"}]}";
    }
    if (_pendingUserTurn.length()) {
        pushHistory(_pendingUserTurn);
        _pendingUserTurn = "";
    }
    pushHistory(modelJson);
    out.text.trim();
    out.ok = true;
    return true;
}

// A fresh user turn must not follow a model turn that made a functionCall with no
// functionResponse after it (the API rejects that). This happens when a tool follow-up
// failed (rate limit / network) last turn. Drop such dangling tool-call turns.
void DianaGemini::dropTrailingToolCall() {
    while (!_history.empty()) {
        const String& last = _history.back();
        bool isModel = last.indexOf("\"role\":\"model\"") >= 0;
        if (isModel && last.indexOf("functionCall") >= 0) {
            _history.pop_back();
            Serial.println("[GEMINI] dropped a dangling tool-call from history (prev turn failed)");
        } else break;
    }
}

bool DianaGemini::generate(const String& userText, const char* audioPath, size_t audioBytes, GeminiResult& out) {
    dropTrailingToolCall();
    String head;
    head.reserve(_systemPrompt.length() + 600);
    head += "{\"systemInstruction\":{\"parts\":[{\"text\":\"";
    head += jsonEscape(_systemPrompt);
    head += "\"}]},\"contents\":[";
    String hist = historyJson();
    head += hist;
    if (hist.length()) head += ',';
    if (audioPath) {
        head += "{\"role\":\"user\",\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/L16;rate=16000\",\"data\":\"";
        String tail = "\"}}";
        if (userText.length()) tail += ",{\"text\":\"" + jsonEscape(userText) + "\"}";
        tail += "]}" + requestTail();
        _pendingUserTurn = "{\"role\":\"user\",\"parts\":[{\"text\":\"[voice message]\"}]}";
        return callModel(head, audioPath, audioBytes, tail, out);
    }
    head += "{\"role\":\"user\",\"parts\":[{\"text\":\"" + jsonEscape(userText) + "\"}]}";
    _pendingUserTurn = "{\"role\":\"user\",\"parts\":[{\"text\":\"" + jsonEscape(userText) + "\"}]}";
    return callModel(head, nullptr, 0, requestTail(), out);
}

bool DianaGemini::sendFunctionResponses(const std::vector<FunctionResponse>& responses, GeminiResult& out) {
    String turn = "{\"role\":\"user\",\"parts\":[";
    for (size_t i = 0; i < responses.size(); ++i) {
        if (i) turn += ',';
        turn += "{\"functionResponse\":{\"name\":\"" + jsonEscape(responses[i].name) + "\"";
        if (responses[i].id.length()) turn += ",\"id\":\"" + jsonEscape(responses[i].id) + "\"";
        turn += ",\"response\":" + responses[i].responseJson + "}}";
    }
    turn += "]}";
    String head;
    head += "{\"systemInstruction\":{\"parts\":[{\"text\":\"";
    head += jsonEscape(_systemPrompt);
    head += "\"}]},\"contents\":[";
    String hist = historyJson();
    head += hist;
    if (hist.length()) head += ',';
    head += turn;
    _pendingUserTurn = turn;
    return callModel(head, nullptr, 0, requestTail(), out);
}

// ─────────────────────────── streaming TTS ───────────────────────────────
bool DianaGemini::ttsStream(const String& text, const char* model, const String& stylePrefix,
                            std::function<void(const uint8_t*, size_t)> onPcm,
                            std::function<bool()> tick, String& err) {
    lastTtsStatus = 0;
    String style = stylePrefix.length() ? stylePrefix : String(Config.ttsStyleOrDefault());
    String body = "{\"contents\":[{\"parts\":[{\"text\":\"";
    body += jsonEscape(style + text);
    body += "\"}]}],\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"";
    body += jsonEscape(Config.ttsVoiceOrDefault());
    body += "\"}}}}}";
    String path = String("/v1beta/models/") + model + ":streamGenerateContent?alt=sse";

    if (!_http.begin(GEMINI_HOST, "POST", path, body.length(), Config.apiKey.c_str())) { err = _http.lastError; Serial.printf("[TTS ERR] TLS connect failed: %s\n", err.c_str()); return false; }
    if (_http.writeStr(body) != body.length()) { err = "tts write failed"; _http.close(); Serial.println("[TTS ERR] request write failed"); return false; }
    int status = _http.finishRequest(30000);
    lastTtsStatus = status;
    if (status < 0) { err = _http.lastError; Serial.printf("[TTS ERR] no response: %s\n", err.c_str()); return false; }
    if (status != 200) {
        String b = _http.readBodyToString(2000);
        bool rate = logGeminiError("tts/streamGenerateContent", model, status, b);
        JsonDocument ed;
        const char* m = (!deserializeJson(ed, b)) ? (ed["error"]["message"] | "") : "";
        err = rate ? (String("TTS rate limit (HTTP ") + status + ")") : (String("TTS HTTP ") + status);
        if (m && *m) err += String(": ") + m;
        return false;
    }

    // Scan the raw SSE body for every  "data":"<base64>"  value and decode it straight
    // to PCM on the fly — no per-line String building (that was too slow and starved
    // the audio feed). One decoder per value; reset on each closing quote.
    static const char KEY[] = "\"data\"";
    int keyPos = 0;
    int state = 0;               // 0=find key, 1=find opening quote, 2=decode until quote
    Base64Decoder dec;
    char in[512];
    uint8_t pcm[512];
    bool anyAudio = false;
    bool aborted = false;
    uint32_t lastTick = millis();
    while (!_http.ended() && !aborted) {
        size_t r = _http.readBytes(in, sizeof(in));
        if (r == 0) break;
        size_t i = 0;
        while (i < r) {
            char c = in[i];
            if (state == 0) {
                keyPos = (c == KEY[keyPos]) ? keyPos + 1 : (c == KEY[0] ? 1 : 0);
                if (KEY[keyPos] == '\0') { state = 1; keyPos = 0; }
                ++i;
            } else if (state == 1) {
                if (c == ':') { /* skip to opening quote */ }
                if (c == '"') { state = 2; dec = Base64Decoder(); }
                ++i;
            } else {
                size_t j = i;
                while (j < r && in[j] != '"') ++j;
                // decode in <=384-char slices so pcm[512] can't overflow (384 b64 -> 288 bytes)
                size_t off = i;
                while (off < j) {
                    size_t take = j - off < 384 ? j - off : 384;
                    size_t n = dec.feed(in + off, take, pcm);
                    if (n) { onPcm(pcm, n); anyAudio = true; }
                    off += take;
                }
                if (j < r) {
                    size_t n = dec.finish(pcm);
                    if (n) { onPcm(pcm, n); anyAudio = true; }
                    state = 0;
                    i = j + 1;
                } else {
                    i = j;
                }
            }
        }
        if (tick && millis() - lastTick > 30) { lastTick = millis(); if (!tick()) aborted = true; }
    }
    _http.drain();
    if (!anyAudio && !aborted) { err = "tts: no audio streamed"; return false; }
    return anyAudio;
}

// ───────────────────────────── transcription ─────────────────────────────
bool DianaGemini::transcribe(const char* audioPath, size_t audioBytes, String& out, String& err) {
    String head = "{\"systemInstruction\":{\"parts\":[{\"text\":\"Transcribe the audio verbatim. Output only the words spoken, lowercase, no punctuation, nothing else.\"}]},"
                  "\"contents\":[{\"role\":\"user\",\"parts\":[{\"inlineData\":{\"mimeType\":\"audio/L16;rate=16000\",\"data\":\"";
    String tail = "\"}}]}],\"generationConfig\":{\"maxOutputTokens\":40,\"temperature\":0}}";
    JsonDocument doc;
    int status = 0;
    if (!sendRequest(Config.chatModelOrDefault(), head, audioPath, audioBytes, tail, doc, status, err)) return false;
    out = "";
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>())
        if (part["text"].is<const char*>()) out += (const char*)part["text"];
    out.trim();
    return out.length() > 0;
}

// ───────────────────────────── grounded search ───────────────────────────
bool DianaGemini::groundedSearch(const String& query, String& answer, String& err) {
    String head = "{\"contents\":[{\"role\":\"user\",\"parts\":[{\"text\":\"";
    head += jsonEscape(query + "\n\nAnswer in at most three plain-text sentences with the key facts.");
    head += "\"}]}";
    String tail = "],\"tools\":[{\"google_search\":{}}],\"generationConfig\":{\"maxOutputTokens\":300}}";
    JsonDocument doc;
    int status = 0;
    if (!sendRequest(Config.chatModelOrDefault(), head, nullptr, 0, tail, doc, status, err)) {
        if (status == 429) err = "Web search grounding is not available on this API plan (quota).";
        return false;
    }
    answer = "";
    for (JsonObject part : doc["candidates"][0]["content"]["parts"].as<JsonArray>()) {
        if (part["text"].is<const char*>()) answer += (const char*)part["text"];
    }
    answer.trim();
    if (answer.isEmpty()) { err = "search returned nothing"; return false; }
    return true;
}
