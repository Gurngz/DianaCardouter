// Streaming base64 encoder/decoder - no large buffers, suitable for
// pushing SD-card PCM into an HTTPS body and pulling PCM out of a JSON reply.
#pragma once
#include <Arduino.h>

class Base64Encoder {
public:
    static size_t encodedLength(size_t bytes) { return 4 * ((bytes + 2) / 3); }

    // Encodes n input bytes, writes complete quads to out (must hold 4*ceil(n/3)+4).
    // Returns chars written. Leftover 1-2 bytes are kept until next call / finish().
    size_t feed(const uint8_t* in, size_t n, char* out) {
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            _pend[_pendLen++] = in[i];
            if (_pendLen == 3) {
                w += encodeTriple(_pend, 3, out + w);
                _pendLen = 0;
            }
        }
        return w;
    }
    size_t finish(char* out) {
        size_t w = 0;
        if (_pendLen) {
            w = encodeTriple(_pend, _pendLen, out);
            _pendLen = 0;
        }
        return w;
    }

private:
    uint8_t _pend[3];
    uint8_t _pendLen = 0;

    static size_t encodeTriple(const uint8_t* b, uint8_t len, char* out) {
        static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        uint32_t v = (uint32_t)b[0] << 16 | (len > 1 ? (uint32_t)b[1] << 8 : 0) | (len > 2 ? b[2] : 0);
        out[0] = T[(v >> 18) & 63];
        out[1] = T[(v >> 12) & 63];
        out[2] = len > 1 ? T[(v >> 6) & 63] : '=';
        out[3] = len > 2 ? T[v & 63] : '=';
        return 4;
    }
};

class Base64Decoder {
public:
    // Decodes n chars, writes bytes to out (must hold 3*n/4+3). Whitespace ignored.
    // Returns bytes written.
    size_t feed(const char* in, size_t n, uint8_t* out) {
        size_t w = 0;
        for (size_t i = 0; i < n; ++i) {
            int v = val(in[i]);
            if (v < 0) continue;              // skip whitespace, padding, junk
            _acc = (_acc << 6) | v;
            if (++_bits == 4) {
                out[w++] = (_acc >> 16) & 0xFF;
                out[w++] = (_acc >> 8) & 0xFF;
                out[w++] = _acc & 0xFF;
                _acc = 0; _bits = 0;
            }
        }
        return w;
    }
    size_t finish(uint8_t* out) {
        size_t w = 0;
        if (_bits == 2) { out[w++] = (_acc >> 4) & 0xFF; }
        else if (_bits == 3) { out[w++] = (_acc >> 10) & 0xFF; out[w++] = (_acc >> 2) & 0xFF; }
        _acc = 0; _bits = 0;
        return w;
    }

private:
    uint32_t _acc = 0;
    uint8_t _bits = 0;
    static int val(char c) {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    }
};
