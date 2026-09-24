// Shared JSON string escaping (one implementation for the REST client, Live client and tools).
#pragma once
#include <Arduino.h>

inline void jsonEscapeTo(String& out, const String& s) {
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

inline String jsonEscaped(const String& s) {
    String o;
    o.reserve(s.length() + 8);
    jsonEscapeTo(o, s);
    return o;
}
