// Holographic HUD, pocket edition: cyan blueprint on black, like web_ui/ on the PC.
//   [status bar: core pulse | DIANA state ........ clock  batt]
//   [scrolling log: YOU / DIANA / SYS / ERR lines]
//   [input line: > typed text_   or   REC meter / thinking spinner]
#pragma once
#include <Arduino.h>
#include <M5Unified.h>
#include <vector>

enum class DianaState { BOOT, SETUP, STANDBY, WAKING, IDLE, RECORDING, THINKING, SPEAKING, OFF };

class DianaUI {
public:
    void begin(bool jpFont, int brightness);
    void setFont(bool jpFont);

    // full-screen scenes
    void showBoot();
    void bootLine(const String& line, bool ok = true);
    void showStandby();
    void setStandbyInput(const String& text);   // typed wake-phrase line on the standby screen
    void showSetup(const String& ssid, const String& url, int clients);
    void showHud();                      // switch to the chat HUD

    // HUD content
    void setState(DianaState s);
    DianaState state() const { return _state; }
    void log(const String& text, char type);   // 'u' you, 'a' diana, 's' sys, 'e' err
    void setInput(const String& text);
    void scroll(int lines);
    void setLevel(int rms) { _level = rms; }
    void setListening(bool on, int level) { if (_listeningHint != on || (on && level != _level)) { _dirtyIn = true; } _listeningHint = on; _level = level; }
    void setStatus(int battery, bool charging, int rssi, bool muted, bool sd);
    void tick();                          // animations; call from loop()
    void redraw();

    bool isHud() const { return _scene == Scene::HUD; }

private:
    enum class Scene { NONE, BOOT, STANDBY, SETUP, HUD };
    struct Line { String text; char type; };

    Scene _scene = Scene::NONE;
    DianaState _state = DianaState::BOOT;
    M5Canvas _log{&M5.Display};
    M5Canvas _bar{&M5.Display};
    M5Canvas _in{&M5.Display};
    std::vector<Line> _lines;
    int  _scroll = 0;
    int  _lineH = 9;
    int  _bootY = 0;
    int  _level = 0;
    int  _battery = -1, _rssi = 0;
    bool _charging = false, _muted = false, _sd = false;
    bool _jp = false;
    bool _listeningHint = false;
    bool _dirtyLog = false, _dirtyIn = false, _dirtyBar = false;
    String _input;
    uint32_t _lastAnim = 0;
    uint32_t _lastBar = 0;
    float _phase = 0;

    const lgfx::IFont* _font = nullptr;
    void drawLog();
    void drawBar();
    void drawInput();
    void drawCore(LovyanGFX& g, int cx, int cy, float phase, int r);
    void wrapAndAdd(const String& text, char type);
    void autoFont(const String& text);   // pick JA/CN glyph font per line (multilingual mode)
    uint32_t colorFor(char type) const;
    const char* stateLabel() const;
    static String clockText();
};

extern DianaUI UI;

// palette (rgb888)
#define C_CYAN     0x00D4FFu
#define C_CYAN_DIM 0x006C82u
#define C_CYAN_DK  0x003644u
#define C_WHITE    0xFFFFFFu
#define C_GREY     0x8A8A8Au
#define C_DGREY    0x4A4A4Au
#define C_RED      0xFF3333u
#define C_AMBER    0xFFB000u
#define C_GREEN    0x2EFF8Au
#define C_BLACK    0x000000u
#define C_BG       0x00080Du
