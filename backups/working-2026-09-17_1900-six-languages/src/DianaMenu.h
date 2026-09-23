// On-device settings menu: WiFi connect, API keys, voice/volume/brightness/hands-free,
// a WiFi analyzer, and IP-based location. Rendered full-screen on M5.Display; navigated
// with ; (up) . (down) , (left) / (right), Enter (select), ` or Del (back/cancel).
#pragma once
#include <Arduino.h>
#include <M5Cardputer.h>
#include <vector>

class DianaMenu {
public:
    void open();
    void close();
    bool isOpen() const { return _open; }
    // Handle one key event; returns true if the menu consumed it (stays open),
    // false when the menu just closed (caller should restore the HUD).
    void handleKeys(const Keyboard_Class::KeysState& st);
    void draw();
    void tick();   // for cursor blink in text fields

private:
    enum Page { MAIN, WIFI, WIFI_PW, KEYS, KEY_ADD, ANALYZER, LOCATE };
    bool _open = false;
    Page _page = MAIN;
    int  _sel = 0;
    int  _top = 0;                 // scroll offset
    bool _dirty = true;
    String _input;                 // text-entry buffer (password / key)
    bool   _pwMask = false;
    String _pendingSsid;
    String _status;                // one-line status/result shown under the list
    std::vector<String> _ssids;
    std::vector<int>    _rssi;
    std::vector<int>    _enc;
    std::vector<int>    _chan;
    uint32_t _lastBlink = 0;

    void enter(Page p);
    void scanWifi();
    void doLocate();
    int  itemCount();
    String itemLabel(int i);
    void activate(int i);          // Enter on item i
    void adjust(int i, int dir);   // left/right on item i
    void drawList(const char* title);
    void drawTextEntry(const char* title, const char* hint);
    void addChar(char c);
};

extern DianaMenu Menu;
