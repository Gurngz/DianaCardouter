// "Pixel Music" — built-in 8-bit chiptune tracks played on the speaker with an animated
// pixel visualizer. Fully offline (no network/quota). Non-blocking: serviced from loop().
#pragma once
#include <Arduino.h>

class DianaMusic {
public:
    void open(int track = 0);   // take over the screen and start playing
    void close();               // stop and hand the screen back
    bool isOpen() const { return _open; }

    // Background control (used by voice tools): returns a status string.
    String play(int track);     // track < 0 = current/first
    String playByName(const String& name);
    String stop();
    String next();
    String list();
    int    trackCount() const;
    String trackName(int i) const;

    void handleKey(const struct Keys& st) {}   // (unused placeholder)
    void handleKeysRaw();       // reads M5Cardputer keyboard directly
    void service();             // advance notes + animate; call every loop while open

private:
    bool     _open = false;
    bool     _playing = false;
    int      _track = 0;
    int      _note = 0;
    uint32_t _noteStart = 0;
    uint16_t _curDur = 0;
    uint32_t _lastDraw = 0;
    int      _lastFreq = 0;
    float    _phase = 0;
    bool     _dirty = true;

    void startNote();
    void drawScreen(bool full);
};

extern DianaMusic Music;
