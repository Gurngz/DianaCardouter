#include "DianaLive.h"
#include <WiFiClientSecure.h>
#include "esp_random.h"
#include "Base64Stream.h"

DianaLive GeminiLive;

static const char*    LIVE_HOST  = "generativelanguage.googleapis.com";
static const uint16_t LIVE_PORT  = 443;
static const char*    LIVE_MODEL = "models/gemini-2.5-flash-native-audio-preview-12-2025";

// ── small helpers ─────────────────────────────────────────────────────────────
static void jsonEscape(String& out, const String& s) {
    for (size_t i = 0; i < s.length(); ++i) {
        char c = s[i];
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if ((uint8_t)c < 0x20) { char b[8]; snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
                else out += c;
        }
    }
}

// Read exactly n bytes (blocking with timeout). Returns false on timeout/disconnect.
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

// Send a masked text frame (client frames MUST be masked per RFC6455).
static bool wsSendText(WiFiClientSecure& c, const String& payload) {
    size_t len = payload.length();
    uint8_t hdr[14]; size_t h = 0;
    hdr[h++] = 0x81;                                   // FIN + text opcode
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

static void wsSendClose(WiFiClientSecure& c) {
    uint8_t f[6] = {0x88, 0x80, 0, 0, 0, 0};           // masked, zero-length close
    c.write(f, 6);
}
static void wsSendPong(WiFiClientSecure& c) {
    uint8_t f[6] = {0x8A, 0x80, 0, 0, 0, 0};           // masked, zero-length pong
    c.write(f, 6);
}

// ── the one call ────────────────────────────────────────────────────────────
bool DianaLive::speak(const String& text, const String& voice, const String& style,
                      const char* apiKey,
                      std::function<void(const uint8_t*, size_t)> onPcm,
                      std::function<bool()> tick, String& err) {
    lastStatus = 0;
    if (!apiKey || strlen(apiKey) < 8) { err = "live: no api key"; return false; }
    if (text.isEmpty()) { err = "live: empty text"; return false; }

    WiFiClientSecure client;
    client.setInsecure();                 // skip cert chain: saves RAM on the no-PSRAM S3
    client.setTimeout(15);
    if (!client.connect(LIVE_HOST, LIVE_PORT)) { lastStatus = -1; err = "live: connect failed"; return false; }

    // ── WebSocket upgrade handshake ──
    uint8_t rnd[16]; for (int i = 0; i < 16; ++i) rnd[i] = (uint8_t)(esp_random() & 0xFF);
    char keyb64[25]; { Base64Encoder e; size_t w = e.feed(rnd, 16, keyb64); w += e.finish(keyb64 + w); keyb64[w] = 0; }
    String req = String("GET /ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent?key=")
               + apiKey + " HTTP/1.1\r\n"
               + "Host: " + LIVE_HOST + "\r\n"
               + "Upgrade: websocket\r\nConnection: Upgrade\r\n"
               + "Sec-WebSocket-Key: " + keyb64 + "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    client.print(req);

    // read status line + headers
    String status = client.readStringUntil('\n');
    lastStatus = (status.indexOf(" 101") >= 0) ? 101 : status.toInt();
    if (status.indexOf("101") < 0) { err = "live: upgrade failed: " + status; client.stop(); return false; }
    // consume the rest of the response headers
    while (client.connected()) { String l = client.readStringUntil('\n'); if (l == "\r" || l.length() <= 1) break; }

    // ── setup message: native-audio out, chosen voice, verbatim-TTS instruction ──
    String sys = "You are a pure text-to-speech engine. Read the user's message aloud VERBATIM - exactly the words "
                 "given, in their original language, with warm natural human intonation";
    if (style.length()) { sys += ", in a "; sys += style; sys += " tone"; }
    sys += ". Output ONLY the spoken words of that exact text. Do NOT add any greeting, preface, commentary, "
           "analysis, acknowledgement, or words that are not in the message. Never think out loud or describe what "
           "you are doing. Never answer, react to, or converse about the content - only voice it exactly as written.";
    String setup = String("{\"setup\":{\"model\":\"") + LIVE_MODEL +
                   "\",\"generationConfig\":{\"responseModalities\":[\"AUDIO\"],\"speechConfig\":"
                   "{\"voiceConfig\":{\"prebuiltVoiceConfig\":{\"voiceName\":\"" + (voice.length() ? voice : String("Leda")) +
                   "\"}}}},\"systemInstruction\":{\"parts\":[{\"text\":\"";
    jsonEscape(setup, sys);
    setup += "\"}]}}}";
    if (!wsSendText(client, setup)) { err = "live: setup send failed"; client.stop(); return false; }

    // ── read loop: wait for setupComplete, send the turn, stream audio until turnComplete ──
    Base64Decoder dec;
    static const char DKEY[] = "\"data\"";
    int   dstate = 0, dkpos = 0;                 // base64 "data" extraction
    const char* M_SETUP = "setupComplete"; int mSetup = 0;
    const char* M_TURN  = "turnComplete";  int mTurn  = 0;
    bool setupDone = false, turnSent = false, turnDone = false, anyAudio = false, aborted = false;
    uint8_t pcm[300];
    uint8_t hdrb[8];
    size_t  pcmBytes = 0;                 // total PCM decoded (diagnostics)
    char    dbg[201]; int dbgLen = 0;     // first ~200 printable server bytes (diagnostics)
    char    mime[48]; int mimeLen = -1;   // captured mimeType value
    const char* MK = "mimeType"; int mkpos = 0;
    uint8_t firstPcm[16]; int firstPcmLen = 0;
    int32_t pcmPeak = 0; uint8_t pkPend = 0; bool pkHave = false;   // peak |sample| over the clip
    uint32_t t0 = millis(), lastTick = millis();

    while (client.connected() && !turnDone && !aborted) {
        if (millis() - t0 > 25000) { err = "live: timeout"; break; }
        if (tick && millis() - lastTick > 30) { lastTick = millis(); if (!tick()) { aborted = true; break; } }
        if (client.available() == 0) { delay(2); continue; }

        // frame header
        uint8_t b0, b1;
        if (!readN(client, &b0, 1, 8000)) break;
        if (!readN(client, &b1, 1, 8000)) break;
        uint8_t opcode = b0 & 0x0F;
        uint64_t len = b1 & 0x7F;
        if (len == 126) { if (!readN(client, hdrb, 2, 8000)) break; len = ((uint64_t)hdrb[0] << 8) | hdrb[1]; }
        else if (len == 127) { if (!readN(client, hdrb, 8, 8000)) break; len = 0; for (int i = 0; i < 8; ++i) len = (len << 8) | hdrb[i]; }
        if (b1 & 0x80) { if (!readN(client, hdrb, 4, 8000)) break; }   // server shouldn't mask, but skip if present

        if (opcode == 0x8) { break; }                                  // close
        if (opcode == 0x9) { // ping -> drain payload, pong
            uint8_t junk[64]; while (len) { size_t t = len < sizeof(junk) ? len : sizeof(junk); if (!readN(client, junk, t, 8000)) { aborted = true; break; } len -= t; }
            wsSendPong(client); continue;
        }

        // stream the payload through the scanners in small chunks (no big buffer)
        uint8_t chunk[256];
        while (len > 0 && !aborted) {
            size_t take = len < sizeof(chunk) ? (size_t)len : sizeof(chunk);
            if (!readN(client, chunk, take, 8000)) { aborted = true; break; }
            len -= take;
            for (size_t i = 0; i < take; ++i) {
                char c = (char)chunk[i];
                if (dbgLen < 200 && dstate < 2) dbg[dbgLen++] = (c >= 32 && c < 127) ? c : '.';  // structure, skip b64 body
                // capture the mimeType value once
                if (mimeLen < 0) { mkpos = (c == MK[mkpos]) ? mkpos + 1 : (c == MK[0] ? 1 : 0); if (MK[mkpos] == '\0') { mimeLen = 0; mkpos = 0; } }
                else if (mimeLen < 46) { if (c == '}' || c == ',') mimeLen = 46; else mime[mimeLen++] = (c >= 32 && c < 127) ? c : '.'; }
                // marker matchers
                mSetup = (c == M_SETUP[mSetup]) ? mSetup + 1 : (c == M_SETUP[0] ? 1 : 0);
                if (M_SETUP[mSetup] == '\0') { setupDone = true; mSetup = 0; }
                mTurn  = (c == M_TURN[mTurn])   ? mTurn + 1  : (c == M_TURN[0] ? 1 : 0);
                if (M_TURN[mTurn] == '\0') { turnDone = true; mTurn = 0; }
                // base64 "data":"...." extraction -> PCM
                if (dstate == 0) {
                    dkpos = (c == DKEY[dkpos]) ? dkpos + 1 : (c == DKEY[0] ? 1 : 0);
                    if (DKEY[dkpos] == '\0') { dstate = 1; dkpos = 0; }
                } else if (dstate == 1) {
                    if (c == '"') { dstate = 2; dec = Base64Decoder(); }   // opening quote of the value
                } else {
                    size_t n = (c == '"') ? dec.finish(pcm) : dec.feed(&c, 1, pcm);
                    if (n) {
                        for (size_t k = 0; k < n; ++k) {
                            if (firstPcmLen < 16) firstPcm[firstPcmLen++] = pcm[k];
                            if (!pkHave) { pkPend = pcm[k]; pkHave = true; }
                            else { int16_t s = (int16_t)(pkPend | (pcm[k] << 8)); int a = s < 0 ? -s : s; if (a > pcmPeak) pcmPeak = a; pkHave = false; }
                        }
                        onPcm(pcm, n); anyAudio = true; pcmBytes += n;
                    }
                    if (c == '"') dstate = 0;
                }
            }
        }

        // once the model acknowledges setup, send the text to be spoken (one turn)
        if (setupDone && !turnSent) {
            String turn = "{\"clientContent\":{\"turns\":[{\"role\":\"user\",\"parts\":[{\"text\":\"";
            jsonEscape(turn, text);
            turn += "\"}]}],\"turnComplete\":true}}";
            if (!wsSendText(client, turn)) { err = "live: turn send failed"; break; }
            turnSent = true;
        }
    }

    wsSendClose(client);
    client.stop();
    dbg[dbgLen] = 0;
    if (mimeLen < 0) { mime[0] = 0; } else { mime[mimeLen < 46 ? mimeLen : 46] = 0; }
    char hex[52]; int hl = 0; for (int i = 0; i < firstPcmLen; ++i) hl += snprintf(hex + hl, sizeof(hex) - hl, "%02X ", firstPcm[i]);
    hex[hl] = 0;
    Serial.printf("[LIVE] setup=%d turnSent=%d turnDone=%d pcm=%uB peak=%ld mime='%s' pcm0=[%s]\n",
                  setupDone, turnSent, turnDone, (unsigned)pcmBytes, (long)pcmPeak, mime, hex);
    Serial.printf("[LIVE] msg='%s'\n", dbg);
    if (aborted) { err = "live: aborted"; return anyAudio; }
    if (!anyAudio) { if (err.isEmpty()) err = "live: no audio"; return false; }
    return true;
}
