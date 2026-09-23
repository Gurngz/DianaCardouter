#include "DianaHttp.h"
#include "config.h"

String HttpsStream::_caPem;

HttpsStream::HttpsStream() {}

void HttpsStream::setCACert(const String& pem) { _caPem = pem; }

bool HttpsStream::connectIfNeeded(const char* host) {
    if (_client.connected() && _host == host && _ended) {
        return true;                                   // keep-alive reuse
    }
    _client.stop();
    _host = host;
    if (_caPem.length()) _client.setCACert(_caPem.c_str());
    else _client.setInsecure();
    _client.setHandshakeTimeout(20);
    _client.setTimeout(15);
    uint32_t t0 = millis();
    if (!_client.connect(host, 443)) {
        lastError = "TLS connect failed";
        Serial.printf("[HTTP] connect to %s failed after %lu ms\n", host, (unsigned long)(millis() - t0));
        return false;
    }
    Serial.printf("[HTTP] connected to %s in %lu ms (heap %u)\n", host, (unsigned long)(millis() - t0), ESP.getFreeHeap());
    return true;
}

bool HttpsStream::begin(const char* host, const char* method, const String& path, size_t contentLength_,
                        const char* apiKey, const char* contentType) {
    lastError = "";
    lastStatus = 0;
    if (!connectIfNeeded(host)) return false;
    String hdr;
    hdr.reserve(256 + path.length());
    hdr += method; hdr += ' '; hdr += path; hdr += " HTTP/1.1\r\n";
    hdr += "Host: "; hdr += host; hdr += "\r\n";
    hdr += "User-Agent: Diana-Cardputer/" DIANA_VERSION "\r\n";
    hdr += "Connection: keep-alive\r\n";
    hdr += "Accept: application/json\r\n";
    if (apiKey && *apiKey) { hdr += "x-goog-api-key: "; hdr += apiKey; hdr += "\r\n"; }
    if (contentLength_ > 0 || strcmp(method, "POST") == 0) {
        hdr += "Content-Type: "; hdr += contentType; hdr += "\r\n";
        hdr += "Content-Length: "; hdr += String(contentLength_); hdr += "\r\n";
    }
    hdr += "\r\n";
    size_t w = _client.write((const uint8_t*)hdr.c_str(), hdr.length());
    if (w != hdr.length()) {
        lastError = "header write failed";
        _client.stop();
        return false;
    }
    _ended = false;   // a request is in flight
    return true;
}

size_t HttpsStream::write(uint8_t c) { return _client.write(&c, 1); }

size_t HttpsStream::write(const uint8_t* buf, size_t n) {
    size_t total = 0;
    while (total < n) {
        size_t chunk = n - total;
        if (chunk > 1024) chunk = 1024;
        size_t w = _client.write(buf + total, chunk);
        if (w == 0) {
            if (!_client.connected()) break;
            delay(1);
            continue;
        }
        total += w;
    }
    return total;
}

size_t HttpsStream::readRaw(uint8_t* buf, size_t n, uint32_t timeoutMs) {
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        int avail = _client.available();
        if (avail > 0) {
            int r = _client.read(buf, n);
            if (r > 0) return (size_t)r;
        } else if (!_client.connected()) {
            return 0;
        }
        delay(1);
    }
    return 0;
}

bool HttpsStream::readLine(String& line, uint32_t timeoutMs) {
    line = "";
    uint8_t c;
    uint32_t t0 = millis();
    while (millis() - t0 < timeoutMs) {
        if (readRaw(&c, 1, timeoutMs) != 1) return false;
        if (c == '\n') return true;
        if (c != '\r') line += (char)c;
        if (line.length() > 2048) return false;
    }
    return false;
}

int HttpsStream::finishRequest(uint32_t firstByteTimeoutMs) {
    String line;
    if (!readLine(line, firstByteTimeoutMs)) {
        lastError = "no response (timeout)";
        _client.stop();
        _ended = true;
        return -1;
    }
    // "HTTP/1.1 200 OK"
    int sp = line.indexOf(' ');
    lastStatus = (sp > 0) ? line.substring(sp + 1, sp + 4).toInt() : -2;
    chunked = false;
    _haveLength = false;
    contentLength = 0;
    _keepAlive = true;
    while (readLine(line, 10000)) {
        if (line.length() == 0) break;
        String lower = line;
        lower.toLowerCase();
        if (lower.startsWith("content-length:")) {
            contentLength = (size_t)lower.substring(15).toInt();
            _haveLength = true;
        } else if (lower.startsWith("transfer-encoding:") && lower.indexOf("chunked") > 0) {
            chunked = true;
        } else if (lower.startsWith("connection:") && lower.indexOf("close") > 0) {
            _keepAlive = false;
        }
    }
    _remaining = chunked ? 0 : contentLength;
    _ended = (!chunked && _haveLength && contentLength == 0);
    return lastStatus;
}

bool HttpsStream::readChunkHeader() {
    String line;
    // tolerate a blank line left over from the previous chunk
    for (int i = 0; i < 3; ++i) {
        if (!readLine(line, 15000)) return false;
        if (line.length()) break;
    }
    _remaining = (size_t)strtoul(line.c_str(), nullptr, 16);
    if (_remaining == 0) {
        // trailer(s) then blank line
        while (readLine(line, 5000) && line.length()) {}
        _ended = true;
        if (!_keepAlive) _client.stop();
        return false;
    }
    return true;
}

size_t HttpsStream::readBytes(char* buffer, size_t length) {
    size_t got = 0;
    while (got < length && !_ended) {
        if (chunked) {
            if (_remaining == 0 && !readChunkHeader()) break;
        } else if (_haveLength) {
            if (_remaining == 0) { _ended = true; if (!_keepAlive) _client.stop(); break; }
        }
        size_t want = length - got;
        if ((chunked || _haveLength) && want > _remaining) want = _remaining;
        size_t r = readRaw((uint8_t*)buffer + got, want, 15000);
        if (r == 0) {
            // connection closed or stalled
            _ended = true;
            if (!chunked && !_haveLength) break;   // read-until-close: normal end
            lastError = "body read stalled";
            _client.stop();
            break;
        }
        got += r;
        if (chunked || _haveLength) {
            _remaining -= r;
            if (chunked && _remaining == 0) {
                String crlf;
                readLine(crlf, 5000);          // consume the CRLF after the chunk
            }
        }
    }
    return got;
}

int HttpsStream::read() {
    char c;
    return readBytes(&c, 1) == 1 ? (unsigned char)c : -1;
}

int HttpsStream::available() {
    if (_ended) return 0;
    int a = _client.available();
    return a > 0 ? a : (_client.connected() ? 0 : 0);
}

void HttpsStream::drain() {
    uint8_t buf[256];
    uint32_t t0 = millis();
    while (!_ended && millis() - t0 < 5000) {
        if (readBytes(buf, sizeof(buf)) == 0) break;
    }
    if (!_ended) _client.stop();
    _ended = true;
}

void HttpsStream::close() {
    _client.stop();
    _ended = true;
}

String HttpsStream::readBodyToString(size_t maxLen) {
    String s;
    s.reserve(1024);
    char buf[256];
    while (!_ended && s.length() < maxLen) {
        size_t r = readBytes(buf, sizeof(buf));
        if (r == 0) break;
        for (size_t i = 0; i < r; ++i) s += buf[i];
    }
    drain();
    return s;
}
