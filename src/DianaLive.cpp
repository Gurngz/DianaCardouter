#include "DianaLive.h"
#include "esp_random.h"
#include "Base64Stream.h"
#include "DianaJson.h"
#include "DianaHttp.h"
#include "config.h"

DianaLive GeminiLive;

static const char*    LIVE_HOST  = GEMINI_HOST;
static const uint16_t LIVE_PORT  = 443;

// ── small helpers ─────────────────────────────────────────────────────────────
static inline void jsonEscape(String& out, const String& s) { jsonEscapeTo(out, s); }

static bool readN(WiFiClientSecure& c, uint8_t* buf, size_t n, uint32_t toMs) {
    size_t got = 0; uint32_t t0 = millis();
    while (got < n) {
        int r = c.read(buf + got, n - got);
        if (r > 0) { got += r; t0 = millis(); continue; }
        if (!c.connected() && c.available() == 0) return false;
        if (millis() - t0 > toMs) return false;
        delay(1);
    }
    return true;
}

// Read a WebSocket frame header; returns opcode and payload length. Server frames aren't masked.
static bool wsFrameHeader(WiFiClientSecure& c, uint8_t& opcode, uint64_t& len, uint32_t toMs) {
    uint8_t b0, b1, hb[8];
    if (!readN(c, &b0, 1, toMs)) return false;
    if (!readN(c, &b1, 1, toMs)) return false;
    opcode = b0 & 0x0F;
    len = b1 & 0x7F;
    if (len == 126) { if (!readN(c, hb, 2, toMs)) return false; len = ((uint64_t)hb[0] << 8) | hb[1]; }
    else if (len == 127) { if (!readN(c, hb, 8, toMs)) return false; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | hb[i]; }
    if (b1 & 0x80) { if (!readN(c, hb, 4, toMs)) return false; }   // (server shouldn't mask)
    return true;
}

static bool wsSendText(WiFiClientSecure& c, const String& payload) {
    size_t len = payload.length();
    uint8_t hdr[14]; size_t h = 0;
    hdr[h++] = 0x81;
    if (len < 126)        hdr[h++] = 0x80 | (uint8_t)len;
    else if (len < 65536){ hdr[h++] = 0x80 | 126; hdr[h++] = (len >> 8) & 0xFF; hdr[h++] = len & 0xFF; }
    else { hdr[h++] = 0x80 | 127; for (int i = 7; i >= 0; --i) hdr[h++] = (len >> (8 * i)) & 0xFF; }
    uint8_t mask[4]; for (int i = 0; i < 4; ++i) mask[i] = (uint8_t)(esp_random() & 0xFF);
    for (int i = 0; i < 4; ++i) hdr[h++] = mask[i];
    if (c.write(hdr, h) != h) return false;
    const char* p = payload.c_str();
    uint8_t buf[128]; size_t off = 0;
    while (off < len) {
        size_t take = (len - off < sizeof(buf)) ? (len - off) : sizeof(buf);
        for (size_t i = 0; i < take; ++i) buf[i] = (uint8_t)p[off + i] ^ mask[(off + i) & 3];
        if (c.write(buf, take) != take) return false;
        off += take;
    }
    return true;
}

static void wsSendPong(WiFiClientSecure& c) { uint8_t f[6] = {0x8A, 0x80, 0, 0, 0, 0}; c.write(f, 6); }

void DianaLive::closeSession() {
    if (_ws.connected()) { uint8_t f[6] = {0x88, 0x80, 0, 0, 0, 0}; _ws.write(f, 6); }
    _ws.stop();
    _ready = false;
}
void DianaLive::end() { closeSession(); }

// Called from the main loop between turns: drains any stray frames and pings the open session
// every few seconds so the Live server doesn't drop it as idle (which caused slow reopens).
void DianaLive::keepAlive() {
    if (!_ready || !_ws.connected()) return;
    while (_ws.available()) {                       // clear unsolicited frames (pongs, etc.)
        uint8_t op; uint64_t len;
        if (!wsFrameHeader(_ws, op, len, 300)) { closeSession(); return; }
        uint8_t jb[128]; while (len) { size_t t = len < sizeof(jb) ? len : sizeof(jb); if (!readN(_ws, jb, t, 300)) { closeSession(); return; } len -= t; }
        if (op == 0x8) { closeSession(); return; }  // server closed the session
        if (op == 0x9) wsSendPong(_ws);             // answer server ping
    }
    if (millis() - _lastPing > 4000) {
        uint8_t ping[6] = {0x89, 0x80, 0, 0, 0, 0}; // masked empty client ping
        if (_ws.write(ping, 6) != 6) { closeSession(); return; }
        _lastPing = millis();
    }
}

bool DianaLive::usable(const String& voice, const String& style, const String& langCode, const char* apiKey) {
    if (!(_ready && _ws.connected() && _key == apiKey && _voice == voice && _style == style)) return false;
    // An empty langCode (model didn't tag the language this turn) keeps the open session, so a
    // missing tag never forces a slow reopen; only an explicit DIFFERENT language reopens.
    return langCode.isEmpty() || _lang == langCode;
}

// Open a fresh session: TLS + WS handshake + setup, then wait for setupComplete. Slow (~2 s),
// done once; speak() reuses the open session afterwards.
bool DianaLive::openSession(const String& voice, const String& style, const String& langCode, const char* apiKey, String& err) {
    closeSession();
    if (HttpsStream::caCert().length()) _ws.setCACert(HttpsStream::caCert().c_str());   // honour /diana/ca.pem like the REST client
    else _ws.setInsecure();
    _ws.setTimeout(15);
    if (!_ws.connect(LIVE_HOST, LIVE_PORT)) { lastStatus = -1; err = "live: connect failed"; return false; }

    uint8_t rnd[16]; for (int i = 0; i < 16; ++i) rnd[i] = (uint8_t)(esp_random() & 0xFF);
    char keyb64[25]; { Base64Encoder e; size_t w = e.feed(rnd, 16, keyb64); w += e.finish(keyb64 + w); keyb64[w] = 0; }
    String req = String("GET /ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=")
               + apiKey + " HTTP/1.1\r\nHost: " + LIVE_HOST + "\r\n"
               + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               + "Sec-WebSocket-Key: " + keyb64 + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    _ws.print(req);
    String status = _ws.readStringUntil('\n');
    // "HTTP/1.1 403 Forbidden" -> 403 (toInt() on the whole line always gave 0)
    { int sp = status.indexOf(' '); lastStatus = sp > 0 ? status.substring(sp + 1).toInt() : 0; }
    if (status.indexOf("101") < 0) { err = "live: upgrade failed: " + status; closeSession(); return false; }
    while (_ws.connected()) { String l = _ws.readStringUntil('\n'); if (l == "\r" || l.length() <= 1) break; }

    String sys = "You are a pure text-to-speech engine. Read the user's message aloud VERBATIM - exactly the words "
                 "given, in their original language, with warm, natural, expressive human intonation that fits the "
                 "feeling of the words themselves";
    if (style.length()) { sys += ", in a "; sys += style; sys += " tone"; }
    sys += ". Output ONLY the spoken words of that exact text. Do NOT add any greeting, preface, commentary, "
           "analysis, acknowledgement, or words that are not in the message. Never think out loud or describe what "
           "you are doing. Never answer, react to, or converse about the content - only voice it exactly as written.";
    // speechConfig carries the prebuilt voice AND (crucially) the target languageCode, so the
    // voice speaks with a NATIVE accent instead of an English-leaning default.
    String speechCfg = String("\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"") +
                       jsonEscaped(voice.length() ? voice : String(DEFAULT_TTS_VOICE)) + "\"}}";
    if (langCode.length()) speechCfg += String(",\"languageCode\":\"") + jsonEscaped(langCode) + "\"";
    String setup = String("{\"setup\":{\"model\":\"") + LIVE_MODEL +
                   "\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":{" + speechCfg +
                   "}},\"systemInstruction\":{\"parts\":[{\"text\":\"";
    jsonEscape(setup, sys);
    setup += "\"}]}}}";
    if (!wsSendText(_ws, setup)) { err = "live: setup send failed"; closeSession(); return false; }

    // wait for setupComplete
    const char* M = "setupComplete"; int mp = 0; bool done = false;
    uint32_t t0 = millis(); uint8_t chunk[128];
    while (_ws.connected() && !done) {
        if (millis() - t0 > 8000) { err = "live: setup timeout"; closeSession(); return false; }
        if (_ws.available() == 0) { delay(2); continue; }
        uint8_t op; uint64_t len;
        if (!wsFrameHeader(_ws, op, len, 8000)) { closeSession(); err = "live: setup read"; return false; }
        if (op == 0x8) { closeSession(); err = "live: closed"; return false; }
        if (op == 0x9) { uint8_t j[64]; while (len) { size_t t = len < sizeof(j) ? len : sizeof(j); if (!readN(_ws, j, t, 8000)) break; len -= t; } wsSendPong(_ws); continue; }
        while (len > 0) {
            size_t take = len < sizeof(chunk) ? (size_t)len : sizeof(chunk);
            if (!readN(_ws, chunk, take, 8000)) { closeSession(); err = "live: setup read"; return false; }
            len -= take;
            for (size_t i = 0; i < take; ++i) { char c = (char)chunk[i]; mp = (c == M[mp]) ? mp + 1 : (c == M[0] ? 1 : 0); if (M[mp] == '\0') { done = true; mp = 0; } }
        }
    }
    _ready = true; _key = apiKey; _voice = voice; _style = style; _lang = langCode;
    return true;
}

// ── speak one turn on the (reused) session ────────────────────────────────────
bool DianaLive::speak(const String& text, const String& voice, const String& style, const String& langCode,
                      const char* apiKey,
                      std::function<void(const uint8_t*, size_t)> onPcm,
                      std::function<bool()> tick, String& err) {
    lastStatus = 0;
    if (!apiKey || strlen(apiKey) < 8) { err = "live: no api key"; return false; }
    if (text.isEmpty()) { err = "live: empty text"; return false; }

    bool reused = usable(voice, style, langCode, apiKey);
    if (!reused) { if (!openSession(voice, style, langCode, apiKey, err)) return false; }

    // send the turn
    String turn = "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":\"";
    jsonEscape(turn, text);
    turn += "\"}]}],\"turnComplete\":true}}";
    if (!wsSendText(_ws, turn)) { closeSession(); err = "live: turn send failed"; return false; }

    // read frames, decode "data" base64 -> PCM, until turnComplete
    Base64Decoder dec;
    static const char DKEY[] = "\"data\"";
    int dstate = 0, dkpos = 0;
    const char* M_TURN = "turnComplete"; int mTurn = 0;
    bool turnDone = false, anyAudio = false, aborted = false;
    uint8_t pcm[300], chunk[256];
    size_t pcmBytes = 0;
    int32_t pcmPeak = 0; uint8_t pkPend = 0; bool pkHave = false;
    uint32_t t0 = millis(), lastTick = millis();

    while (_ws.connected() && !turnDone && !aborted) {
        if (millis() - t0 > 25000) { err = "live: timeout"; break; }
        if (!anyAudio && millis() - t0 > 12000) { err = "live: no audio (timeout)"; break; }  // some languages are slow to first audio; don't cut them off early
        if (tick && millis() - lastTick > 30) { lastTick = millis(); if (!tick()) { aborted = true; break; } }
        if (_ws.available() == 0) { delay(2); continue; }
        uint8_t op; uint64_t len;
        if (!wsFrameHeader(_ws, op, len, 10000)) break;
        if (op == 0x8) break;
        if (op == 0x9) { uint8_t j[64]; while (len) { size_t t = len < sizeof(j) ? len : sizeof(j); if (!readN(_ws, j, t, 8000)) { aborted = true; break; } len -= t; } wsSendPong(_ws); continue; }
        while (len > 0 && !aborted) {
            size_t take = len < sizeof(chunk) ? (size_t)len : sizeof(chunk);
            if (!readN(_ws, chunk, take, 10000)) { aborted = true; break; }
            len -= take;
            for (size_t i = 0; i < take; ++i) {
                char c = (char)chunk[i];
                mTurn = (c == M_TURN[mTurn]) ? mTurn + 1 : (c == M_TURN[0] ? 1 : 0);
                // Only honor turnComplete once audio has actually flowed - this ignores a leftover
                // turnComplete frame from the previous turn bleeding into this one (the empty-turn bug).
                if (M_TURN[mTurn] == '\0') { mTurn = 0; if (anyAudio) turnDone = true; }
                if (dstate == 0) { dkpos = (c == DKEY[dkpos]) ? dkpos + 1 : (c == DKEY[0] ? 1 : 0); if (DKEY[dkpos] == '\0') { dstate = 1; dkpos = 0; } }
                else if (dstate == 1) { if (c == '"') { dstate = 2; dec = Base64Decoder(); } }
                else {
                    size_t n = (c == '"') ? dec.finish(pcm) : dec.feed(&c, 1, pcm);
                    if (n) {
                        for (size_t k = 0; k < n; ++k) { if (!pkHave) { pkPend = pcm[k]; pkHave = true; } else { int16_t s = (int16_t)(pkPend | (pcm[k] << 8)); int a = s < 0 ? -s : s; if (a > pcmPeak) pcmPeak = a; pkHave = false; } }
                        onPcm(pcm, n); anyAudio = true; pcmBytes += n;
                    }
                    if (c == '"') dstate = 0;
                }
            }
        }
    }

    // Drain any residual frames the server sent right after turnComplete (metadata, etc.) so the
    // NEXT reused turn starts at a clean frame boundary and doesn't inherit a stray turnComplete.
    if (turnDone && anyAudio) {
        uint32_t d0 = millis();
        while (_ws.connected() && millis() - d0 < 200 && _ws.available()) {
            uint8_t op; uint64_t l;
            if (!wsFrameHeader(_ws, op, l, 200)) break;
            uint8_t jb[128]; while (l) { size_t t = l < sizeof(jb) ? l : sizeof(jb); if (!readN(_ws, jb, t, 200)) break; l -= t; }
        }
    }
    Serial.printf("[LIVE] reused=%d turnDone=%d pcm=%uB peak=%ld heap=%u\n",
                  reused, turnDone, (unsigned)pcmBytes, (long)pcmPeak, (unsigned)ESP.getFreeHeap());

    if (aborted) { closeSession(); err = "live: aborted"; return anyAudio; }
    if (!turnDone || !anyAudio) { closeSession(); if (err.isEmpty()) err = "live: no audio"; return anyAudio && turnDone; }
    return true;   // keep the session open for the next reply
}
