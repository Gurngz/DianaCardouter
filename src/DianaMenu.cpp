#include "DianaMenu.h"
#include "DianaUI.h"
#include "DianaConfig.h"
#include "DianaNet.h"
#include "DianaAudio.h"
#include "DianaSetup.h"
#include "config.h"
#include <WiFi.h>

DianaMenu Menu;

static const int ROW_H = 14;
static const int LIST_TOP = 20;
static const int VISIBLE = 7;

void DianaMenu::open() {
    _open = true;
    _page = MAIN;
    _sel = 0;
    _top = 0;
    _input = "";
    _status = "";
    _dirty = true;
    draw();
}

void DianaMenu::close() {
    _open = false;
}

void DianaMenu::enter(Page p) {
    _page = p;
    _sel = 0;
    _top = 0;
    _input = "";
    _dirty = true;
    if (p == WIFI || p == ANALYZER) scanWifi();
    if (p == LOCATE) doLocate();
    draw();
}

void DianaMenu::scanWifi() {
    _status = "scanning...";
    draw();
    WiFi.mode(WIFI_STA);
    int n = WiFi.scanNetworks(false, true);   // include hidden
    _ssids.clear(); _rssi.clear(); _enc.clear(); _chan.clear();
    int chCount[15] = {0};
    int hidden = 0, open = 0;
    for (int i = 0; i < n && i < 24; ++i) {
        String s = WiFi.SSID(i);
        int ch = WiFi.channel(i);
        if (ch >= 1 && ch <= 14) chCount[ch]++;
        if ((int)WiFi.encryptionType(i) == 0) open++;
        if (s.isEmpty()) { hidden++; continue; }
        bool dup = false;
        for (auto& e : _ssids) if (e == s) { dup = true; break; }
        if (dup) continue;
        _ssids.push_back(s);
        _rssi.push_back(WiFi.RSSI(i));
        _enc.push_back((int)WiFi.encryptionType(i));
        _chan.push_back(ch);
    }
    WiFi.scanDelete();
    int busy = 1;
    for (int c = 2; c <= 14; ++c) if (chCount[c] > chCount[busy]) busy = c;
    _status = String(n) + " APs, busiest ch " + busy + ", " + hidden + " hidden, " + open + " open";
}

void DianaMenu::doLocate() {
    _status = "locating...";
    draw();
    if (!Net.isConnected()) { _status = "no WiFi"; return; }
    if (Net.locate()) _status = "located";
    else _status = "locate failed";
}

// ─────────────────────────── item model ──────────────────────────────────
int DianaMenu::itemCount() {
    switch (_page) {
        case MAIN:     return 11;
        case WIFI:     return 2 + (int)_ssids.size();   // rescan, back, then networks
        case KEYS:     return 3 + Config.apiKeyCount(); // add, next, back, then keys
        case ANALYZER: return 1 + (int)_ssids.size();   // back + networks
        case LOCATE:   return 1;                        // back
        default:       return 0;
    }
}

static const char* encName(int e) {
    switch (e) { case 0: return "open"; case 2: return "WPA"; case 3: return "WPA2"; case 4: return "WPA2"; case 5: return "WPA3"; }
    return "sec";
}

String DianaMenu::itemLabel(int i) {
    if (_page == MAIN) {
        switch (i) {
            case 0: return String("WiFi: ") + (Net.isConnected() ? Net.ssid() : String("(disconnected)"));
            case 1: return String("API keys: ") + Config.apiKeyCount() + "  (#" + Config.apiKeyIndex + ")";
            case 2: return String("Voice out: ") + (Config.voiceEnabled ? "on" : "off");
            case 3: return String("Hands-free: ") + (Config.handsFree ? "on" : "off");
            case 4: return String("Wake by voice: ") + (Config.voiceWake ? "on" : "off");
            case 5: return String("Volume: ") + Config.volume;
            case 6: return String("Brightness: ") + Config.brightness;
            case 7: return "WiFi analyzer";
            case 8: return "Where am I";
            case 9: return "Setup portal (phone)";
            case 10: return "Close";
        }
    } else if (_page == WIFI) {
        if (i == 0) return "[ rescan ]";
        if (i == 1) return "[ back ]";
        int k = i - 2;
        if (k < (int)_ssids.size()) return _ssids[k] + "  " + _rssi[k] + "dB " + encName(_enc[k]);
    } else if (_page == ANALYZER) {
        if (i == 0) return "[ back ]";
        int k = i - 1;
        if (k < (int)_ssids.size()) {
            return _ssids[k] + "  ch" + (k < (int)_chan.size() ? _chan[k] : 0) + " " + _rssi[k] + "dBm " + encName(_enc[k]);
        }
    } else if (_page == KEYS) {
        if (i == 0) return "[ add key ]";
        if (i == 1) return String("[ next key -> now #") + Config.apiKeyIndex + " ]";
        if (i == 2) return "[ back ]";
        int k = i - 3;
        if (k < Config.apiKeyCount()) {
            String key = Config.apiKeys[k];
            String tail = key.length() > 4 ? key.substring(key.length() - 4) : key;
            return String(k == Config.apiKeyIndex ? "* " : "  ") + "key " + k + " ..." + tail + "  [ENTER=remove]";
        }
    } else if (_page == LOCATE) {
        if (i == 0) return "[ back ]";
    }
    return "";
}

// ─────────────────────────── actions ─────────────────────────────────────
void DianaMenu::activate(int i) {
    if (_page == MAIN) {
        switch (i) {
            case 0: enter(WIFI); return;
            case 1: enter(KEYS); return;
            case 2: Config.voiceEnabled = !Config.voiceEnabled; Config.save(true); break;
            case 3: Config.handsFree = !Config.handsFree; Config.save(true); break;
            case 4: Config.voiceWake = !Config.voiceWake; Config.save(true); break;
            case 5: case 6: break;  // volume/brightness adjust with ,/  not Enter
            case 7: enter(ANALYZER); return;
            case 8: enter(LOCATE); return;
            case 9: close(); _setupReq = true; return;   // main.cpp opens the portal (disconnects TLS first, handles failure)
            case 10: close(); return;
        }
    } else if (_page == WIFI) {
        if (i == 0) { scanWifi(); }
        else if (i == 1) { enter(MAIN); }
        else {
            int k = i - 2;
            if (k < (int)_ssids.size()) {
                _pendingSsid = _ssids[k];
                if (_enc[k] == 0) {           // open network: connect immediately
                    Config.setWifi(_pendingSsid, ""); Config.save(true);
                    _status = "connecting...";
                    draw();
                    bool ok = Net.connect(9000, nullptr);
                    if (ok) Net.syncTime(Config.tz);
                    _status = ok ? "connected: " + Net.ip() : "failed";
                    _page = MAIN;
                } else {
                    _input = "";
                    _pwMask = true;
                    enter(WIFI_PW);
                    return;
                }
            }
        }
    } else if (_page == KEYS) {
        if (i == 0) { _input = ""; _pwMask = false; enter(KEY_ADD); return; }
        else if (i == 1) { Config.rotateApiKey(); Config.save(true); _status = "active key #" + String(Config.apiKeyIndex); }
        else if (i == 2) { enter(MAIN); return; }
        else { int k = i - 3; if (Config.removeApiKey(k)) { Config.save(true); _status = "removed key " + String(k) + " (" + Config.apiKeyCount() + " left)"; if (_sel >= itemCount()) _sel = itemCount() - 1; } }
    } else if (_page == ANALYZER) {
        if (i == 0) { enter(MAIN); return; }
    } else if (_page == LOCATE) {
        if (i == 0) { enter(MAIN); return; }
    }
    _dirty = true;
}

void DianaMenu::adjust(int i, int dir) {
    if (_page != MAIN) return;
    if (i == 5) { Config.volume = constrain(Config.volume + dir * 15, 0, 255); Audio.setVolume(Config.volume); Config.save(true); }
    else if (i == 6) { Config.brightness = constrain(Config.brightness + dir * 15, 8, 255); M5.Display.setBrightness(Config.brightness); Config.save(true); }
    _dirty = true;
}

void DianaMenu::addChar(char c) {
    if ((unsigned char)c >= 0x20 && _input.length() < 96) { _input += c; _dirty = true; }
}

void DianaMenu::handleKeys(const Keyboard_Class::KeysState& st) {
    if (!_open) return;

    if (_page == WIFI_PW || _page == KEY_ADD) {
        // text entry
        if (st.enter) {
            if (_page == WIFI_PW) {
                Config.setWifi(_pendingSsid, _input); Config.save(true);
                _status = "connecting...";
                _page = WIFI; draw();
                bool ok = Net.connect(9000, nullptr);
                if (ok) Net.syncTime(Config.tz);
                _status = ok ? "connected: " + Net.ip() : "wrong password?";
                _page = MAIN; _sel = 0; _top = 0;
            } else {  // KEY_ADD
                if (_input.length() >= 12) { Config.addApiKey(_input); Config.syncActiveKey(); Config.save(true); _status = "key added (" + String(Config.apiKeyCount()) + ")"; }
                else _status = "too short";
                _page = KEYS; _sel = 0; _top = 0;
            }
            _input = "";
            _dirty = true;
            return;
        }
        if (st.esc || (st.fn && st.del)) { _page = (_page == WIFI_PW) ? WIFI : KEYS; _input = ""; _dirty = true; return; }
        if (st.backspace || st.del) {
            if (_input.length()) { _input.remove(_input.length() - 1); _dirty = true; }
            return;
        }
        for (auto c : st.word) addChar(c);
        return;
    }

    // list navigation
    bool up = st.up, down = st.down, left = st.left, right = st.right, sel = st.enter, back = st.esc;
    for (auto c : st.word) {
        if (c == ';') up = true;
        else if (c == '.') down = true;
        else if (c == ',') left = true;
        else if (c == '/') right = true;
        else if (c == '`') back = true;
    }
    int n = itemCount();
    if (up)   { _sel = (_sel - 1 + n) % n; }
    if (down) { _sel = (_sel + 1) % n; }
    if (left)  adjust(_sel, -1);
    if (right) adjust(_sel, +1);
    if (back) {
        if (_page == MAIN) { close(); return; }
        enter(MAIN);
        return;
    }
    if (sel) { activate(_sel); }
    // keep selection visible
    if (_sel < _top) _top = _sel;
    if (_sel >= _top + VISIBLE) _top = _sel - VISIBLE + 1;
    _dirty = true;
    if (_open) draw();
}

// ─────────────────────────── rendering ───────────────────────────────────
void DianaMenu::drawList(const char* title) {
    auto& d = M5.Display;
    d.fillScreen((uint32_t)C_BLACK);
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    d.drawString(title, 4, 3);
    d.drawFastHLine(0, 15, SCREEN_W, (uint32_t)C_CYAN_DK);
    int n = itemCount();
    for (int r = 0; r < VISIBLE; ++r) {
        int i = _top + r;
        if (i >= n) break;
        int y = LIST_TOP + r * ROW_H;
        bool cursor = (i == _sel);
        if (cursor) {
            d.fillRect(0, y - 1, SCREEN_W, ROW_H, (uint32_t)C_CYAN_DK);
            d.setTextColor((uint32_t)C_WHITE, (uint32_t)C_CYAN_DK);
        } else {
            d.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
        }
        String lbl = itemLabel(i);
        if (lbl.length() > 38) lbl = lbl.substring(0, 38);
        d.drawString((cursor ? ">" : " ") + lbl, 2, y);
    }
    // footer / status
    d.drawFastHLine(0, SCREEN_H - 12, SCREEN_W, (uint32_t)C_CYAN_DK);
    d.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    String foot = _status.length() ? _status : String("; . move  / , adj  ENTER ok  ` back");
    if (foot.length() > 40) foot = foot.substring(0, 40);
    d.drawString(foot, 3, SCREEN_H - 10);
}

void DianaMenu::drawTextEntry(const char* title, const char* hint) {
    auto& d = M5.Display;
    d.fillScreen((uint32_t)C_BLACK);
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    d.drawString(title, 4, 6);
    d.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    d.drawString(hint, 4, 22);
    d.drawRect(4, 44, SCREEN_W - 8, 22, (uint32_t)C_CYAN_DK);
    String shown = _input;
    if (_pwMask) { shown = ""; for (size_t i = 0; i < _input.length(); ++i) shown += '*'; }
    int maxc = 26;
    if (shown.length() > (size_t)maxc) shown = shown.substring(shown.length() - maxc);
    d.setFont(&fonts::AsciiFont8x16);
    d.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BLACK);
    d.drawString(shown, 8, 47);
    int cx = 8 + shown.length() * 8;
    if ((millis() / 500) & 1) d.fillRect(cx, 47, 7, 16, (uint32_t)C_CYAN);
    d.setFont(&fonts::Font0);
    d.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    d.drawString("ENTER = ok    ` = cancel", 4, SCREEN_H - 12);
}

void DianaMenu::draw() {
    if (!_open) return;
    switch (_page) {
        case MAIN:     drawList("DIANA  SETTINGS"); break;
        case WIFI:     drawList("WIFI  -  pick a network"); break;
        case ANALYZER: drawList("WIFI ANALYZER"); break;
        case KEYS:     drawList("API KEYS"); break;
        case LOCATE: {
            auto& d = M5.Display;
            d.fillScreen((uint32_t)C_BLACK);
            d.setTextDatum(top_left);
            d.setFont(&fonts::Font0);
            d.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
            d.drawString("WHERE AM I  (approx, IP-based)", 4, 4);
            d.drawFastHLine(0, 15, SCREEN_W, (uint32_t)C_CYAN_DK);
            d.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BLACK);
            if (Net.located()) {
                d.drawString("City: " + Net.city(), 6, 24);
                d.drawString("IP:   " + Net.ip(), 6, 38);
                d.drawString("(GPS needs a Grove GPS module)", 6, 66);
            } else {
                d.drawString(_status.length() ? _status : "no location", 6, 30);
            }
            d.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
            d.drawString("` back", 4, SCREEN_H - 12);
            break;
        }
        case WIFI_PW:  drawTextEntry(("Password for " + _pendingSsid).c_str(), "type the WiFi password"); break;
        case KEY_ADD:  drawTextEntry("Add Gemini API key", "paste/type a key, then ENTER"); break;
    }
    _dirty = false;
}

void DianaMenu::tick() {
    if (!_open) return;
    if (_page == WIFI_PW || _page == KEY_ADD) {
        if (millis() - _lastBlink > 300) { _lastBlink = millis(); draw(); }
    } else if (_dirty) {
        draw();
    }
}
