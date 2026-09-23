// Minimal streaming HTTPS client.
//  - request body is written in pieces (so a big base64 audio blob never sits in RAM)
//  - response body is exposed as a Stream (de-chunked), so ArduinoJson can parse it
//    directly and the TTS scanner can decode base64 on the fly.
//  - keeps the TLS connection alive between calls to the same host when possible.
#pragma once
#include <Arduino.h>
#include <WiFiClientSecure.h>

class HttpsStream : public Stream {
public:
    HttpsStream();

    // Connects (or reuses the connection) and sends the request line + headers.
    bool begin(const char* host, const char* method, const String& path, size_t contentLength,
               const char* apiKey, const char* contentType = "application/json");

    // Body writing
    size_t write(uint8_t c) override;
    size_t write(const uint8_t* buf, size_t n) override;
    size_t writeStr(const String& s) { return write((const uint8_t*)s.c_str(), s.length()); }

    // Reads status line + headers. Returns HTTP status, or <0 on transport error.
    int finishRequest(uint32_t firstByteTimeoutMs = 30000);

    // Body reading (Stream)
    int available() override;
    int read() override;
    int peek() override { return -1; }
    size_t readBytes(char* buffer, size_t length) override;
    size_t readBytes(uint8_t* buffer, size_t length) { return readBytes((char*)buffer, length); }
    bool ended() const { return _ended; }
    void drain();                 // consume whatever is left of the body
    void close();                 // hard close
    bool isConnected() { return _client.connected(); }

    String readBodyToString(size_t maxLen = 24000);

    String lastError;
    int    lastStatus = 0;
    size_t contentLength = 0;
    bool   chunked = false;

    static void setCACert(const String& pem);   // optional; default is setInsecure()

private:
    WiFiClientSecure _client;
    String _host;
    bool   _ended = true;
    bool   _haveLength = false;
    size_t _remaining = 0;        // bytes left in body (length mode) or in current chunk
    bool   _keepAlive = false;
    static String _caPem;

    bool   connectIfNeeded(const char* host);
    size_t readRaw(uint8_t* buf, size_t n, uint32_t timeoutMs);
    bool   readLine(String& line, uint32_t timeoutMs);
    bool   readChunkHeader();
};
