#include "DianaUI.h"
#include "config.h"
#include <time.h>
#include <math.h>

DianaUI UI;

static const int MAX_LINES = 140;

void DianaUI::begin(bool jpFont, int brightness) {
    _jp = jpFont;
    M5.Display.setRotation(1);
    M5.Display.setBrightness(brightness);
    M5.Display.fillScreen((uint32_t)C_BLACK);
    _font = jpFont ? (const lgfx::IFont*)&fonts::efontJA_10 : (const lgfx::IFont*)&fonts::Font0;

    _log.setColorDepth(8);
    _log.createSprite(SCREEN_W, LOG_H);
    _log.setFont(_font);
    _log.setTextWrap(false);
    _lineH = _log.fontHeight() + 1;

    _bar.setColorDepth(16);
    _bar.createSprite(SCREEN_W, BAR_H);
    _bar.setFont(&fonts::Font0);
    _bar.setTextWrap(false);

    _in.setColorDepth(16);
    _in.createSprite(SCREEN_W, INPUT_H);
    _in.setFont(_font);
    _in.setTextWrap(false);
}

void DianaUI::setFont(bool jpFont) {
    _jp = jpFont;
    _font = jpFont ? (const lgfx::IFont*)&fonts::efontJA_10 : (const lgfx::IFont*)&fonts::Font0;
    _log.setFont(_font);
    _in.setFont(_font);
    _lineH = _log.fontHeight() + 1;
    _dirtyLog = _dirtyIn = true;
}

// ─────────────────────────── scenes ─────────────────────────────────────
static void drawBlueprintGrid() {
    for (int x = 0; x < SCREEN_W; x += 20) M5.Display.drawFastVLine(x, 0, SCREEN_H, (uint32_t)0x00161Cu);
    for (int y = 0; y < SCREEN_H; y += 20) M5.Display.drawFastHLine(0, y, SCREEN_W, (uint32_t)0x00161Cu);
}

void DianaUI::showBoot() {
    _scene = Scene::BOOT;
    M5.Display.startWrite();
    M5.Display.fillScreen((uint32_t)C_BLACK);
    drawBlueprintGrid();
    M5.Display.setTextDatum(top_center);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    M5.Display.drawString("DIANA", SCREEN_W / 2, 6);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor((uint32_t)C_CYAN_DIM, (uint32_t)C_BLACK);
    M5.Display.drawString(DIANA_ID "  //  NEURAL CORE  //  v" DIANA_VERSION, SCREEN_W / 2, 36);
    M5.Display.drawFastHLine(20, 48, SCREEN_W - 40, (uint32_t)C_CYAN_DK);
    M5.Display.setTextDatum(top_left);
    M5.Display.endWrite();
    _bootY = 54;
}

void DianaUI::bootLine(const String& line, bool ok) {
    if (_scene != Scene::BOOT) return;
    if (_bootY > SCREEN_H - 10) {
        // scroll the boot log region up by one line
        M5.Display.fillRect(0, 54, SCREEN_W, SCREEN_H - 54, (uint32_t)C_BLACK);
        _bootY = 54;
    }
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor(ok ? (uint32_t)C_CYAN : (uint32_t)C_RED, (uint32_t)C_BLACK);
    M5.Display.drawString(String(ok ? "> " : "! ") + line, 8, _bootY);
    _bootY += 10;
}

void DianaUI::drawCore(LovyanGFX& g, int cx, int cy, float phase, int r) {
    uint32_t col = C_CYAN;
    switch (_state) {
        case DianaState::STANDBY:   col = C_DGREY; break;
        case DianaState::RECORDING: col = C_RED; break;
        case DianaState::THINKING:  col = C_AMBER; break;
        case DianaState::SPEAKING:  col = C_GREEN; break;
        default: break;
    }
    float p = (sinf(phase) + 1.0f) * 0.5f;       // 0..1
    int rr = r - 1 + (int)roundf(p * 2);
    g.drawCircle(cx, cy, rr, col);
    g.fillCircle(cx, cy, (int)roundf(1 + p * (r - 2)), col);
}

void DianaUI::showStandby() {
    _scene = Scene::STANDBY;
    _state = DianaState::STANDBY;
    M5.Display.startWrite();
    M5.Display.fillScreen((uint32_t)C_BLACK);
    drawBlueprintGrid();
    M5.Display.setTextDatum(top_center);
    M5.Display.setFont(&fonts::Orbitron_Light_32);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    M5.Display.drawString("DIANA", SCREEN_W / 2, 30);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor((uint32_t)C_CYAN_DIM, (uint32_t)C_BLACK);
    M5.Display.drawString(DIANA_ID "  //  STANDBY", SCREEN_W / 2, 72);
    M5.Display.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    M5.Display.drawString("type 'wake up diana' + ENTER  (or any /command)", SCREEN_W / 2, 100);
    M5.Display.setTextDatum(top_left);
    M5.Display.endWrite();
    setStandbyInput("");
}

void DianaUI::setStandbyInput(const String& text) {
    if (_scene != Scene::STANDBY) return;
    M5.Display.fillRect(0, 114, SCREEN_W, 21, (uint32_t)C_BLACK);
    M5.Display.setTextDatum(top_left);
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    String shown = text;
    int maxc = 27;
    if (shown.length() > (size_t)maxc) shown = shown.substring(shown.length() - maxc);
    M5.Display.drawString("> " + shown, 6, 116);
    int cx = 6 + (2 + shown.length()) * 8;
    if ((millis() / 500) & 1) M5.Display.fillRect(cx, 116, 7, 16, (uint32_t)C_CYAN);
    M5.Display.setFont(&fonts::Font0);
}

void DianaUI::showSetup(const String& ssid, const String& psk, const String& url, int clients) {
    _scene = Scene::SETUP;
    _state = DianaState::SETUP;
    M5.Display.startWrite();
    M5.Display.fillScreen((uint32_t)C_BLACK);
    drawBlueprintGrid();
    M5.Display.setTextDatum(top_center);
    M5.Display.setFont(&fonts::Orbitron_Light_24);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    M5.Display.drawString("SETUP", SCREEN_W / 2, 4);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BLACK);
    M5.Display.drawString("1. On your phone, join WiFi:", SCREEN_W / 2, 40);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.drawString(ssid + "  pw " + psk, SCREEN_W / 2, 52);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BLACK);
    M5.Display.drawString("2. Open in the browser:", SCREEN_W / 2, 74);
    M5.Display.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
    M5.Display.setFont(&fonts::AsciiFont8x16);
    M5.Display.drawString(url, SCREEN_W / 2, 86);
    M5.Display.setFont(&fonts::Font0);
    M5.Display.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    M5.Display.drawString(String("clients: ") + clients + "   |   ESC = cancel", SCREEN_W / 2, 118);
    M5.Display.setTextDatum(top_left);
    M5.Display.endWrite();
}

void DianaUI::showHud() {
    _scene = Scene::HUD;
    M5.Display.fillScreen((uint32_t)C_BLACK);
    _dirtyLog = _dirtyIn = _dirtyBar = true;
    redraw();
}

// ─────────────────────────── HUD state ──────────────────────────────────
void DianaUI::setState(DianaState s) {
    if (_state == s) return;
    _state = s;
    _dirtyBar = _dirtyIn = true;
}

const char* DianaUI::stateLabel() const {
    switch (_state) {
        case DianaState::BOOT:      return "BOOT";
        case DianaState::SETUP:     return "SETUP";
        case DianaState::STANDBY:   return "STANDBY";
        case DianaState::WAKING:    return "WAKING";
        case DianaState::IDLE:      return "LISTENING";
        case DianaState::RECORDING: return "RECORDING";
        case DianaState::THINKING:  return "THINKING";
        case DianaState::SPEAKING:  return "SPEAKING";
        case DianaState::OFF:       return "OFFLINE";
    }
    return "";
}

uint32_t DianaUI::colorFor(char type) const {
    switch (type) {
        case 'u': return C_GREY;
        case 'a': return C_CYAN;
        case 'e': return C_RED;
        case 'w': return C_AMBER;
        default:  return C_CYAN_DIM;
    }
}

static size_t utf8Len(unsigned char c) {
    if (c < 0x80) return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;
}

void DianaUI::wrapAndAdd(const String& textIn, char type) {
    const int maxW = SCREEN_W - 4;
    const String indent = "  ";
    String prefix = (type == 'u') ? "> " : (type == 's' || type == 'w') ? "# " : (type == 'e') ? "! " : "";
    String text = textIn;
    text.replace("\r", "");
    bool first = true;
    int start = 0;
    while (true) {
        int nl = text.indexOf('\n', start);
        String para = (nl < 0) ? text.substring(start) : text.substring(start, nl);
        String line = first ? prefix : indent;
        // tokenise: words split on spaces; in JP mode every multibyte glyph is its own token
        std::vector<String> tokens;
        String cur;
        for (size_t i = 0; i < para.length();) {
            size_t cl = utf8Len((unsigned char)para[i]);
            String ch = para.substring(i, i + cl);
            i += cl;
            if (ch == " ") {
                if (cur.length()) { tokens.push_back(cur); cur = ""; }
                tokens.push_back(" ");
            } else if (_jp && cl > 1) {
                if (cur.length()) { tokens.push_back(cur); cur = ""; }
                tokens.push_back(ch);
            } else {
                cur += ch;
            }
        }
        if (cur.length()) tokens.push_back(cur);

        for (auto& tok : tokens) {
            if (tok == " ") {
                if (line.length() > indent.length() || (first && line == prefix)) line += " ";
                continue;
            }
            if (_log.textWidth(line + tok) <= maxW) { line += tok; continue; }
            // does not fit: flush current line if it has content
            if (line.length() > prefix.length() || (!first && line.length() > indent.length())) {
                _lines.push_back({line, type});
                line = indent;
            }
            // hard-break tokens wider than a whole line
            while (_log.textWidth(line + tok) > maxW) {
                size_t j = 0;
                while (j < tok.length()) {
                    size_t wl = utf8Len((unsigned char)tok[j]);
                    if (_log.textWidth(line + tok.substring(0, j + wl)) > maxW) break;
                    j += wl;
                }
                if (j == 0) j = utf8Len((unsigned char)tok[0]);
                _lines.push_back({line + tok.substring(0, j), type});
                tok = tok.substring(j);
                line = indent;
            }
            line += tok;
        }
        _lines.push_back({line, type});
        first = false;
        if (nl < 0) break;
        start = nl + 1;
    }
    while ((int)_lines.size() > MAX_LINES) _lines.erase(_lines.begin());
}

// Pick the glyph font for this line's script: Japanese kana -> efontJA, CJK Han -> efontCN,
// otherwise the default multilingual efont (which also covers Latin/accents). Only active in
// multilingual mode (_jp); if the user forced plain ASCII we leave Font0 alone.
void DianaUI::autoFont(const String& t) {
    if (!_jp) return;
    bool kana = false, han = false;
    int n = t.length(), i = 0;
    while (i < n) {
        unsigned char c = (unsigned char)t[i];
        size_t l = utf8Len(c);
        if (l == 3 && i + 2 < n) {
            uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | (((unsigned char)t[i + 1] & 0x3F) << 6) | ((unsigned char)t[i + 2] & 0x3F);
            if (cp >= 0x3040 && cp <= 0x30FF) { kana = true; break; }        // hiragana/katakana => Japanese
            if (cp >= 0x4E00 && cp <= 0x9FFF) han = true;                    // CJK Han
        }
        i += l;
    }
    const lgfx::IFont* want = kana ? (const lgfx::IFont*)&fonts::efontJA_10
                          : han    ? (const lgfx::IFont*)&fonts::efontCN_10
                          :          (const lgfx::IFont*)&fonts::efontJA_10;
    if (want != _font) { _font = want; _log.setFont(_font); _in.setFont(_font); }
}

void DianaUI::log(const String& text, char type) {
    if (!text.length()) return;
    Serial.printf("[%c] %s\n", type, text.c_str());
    if (type == 'a' || type == 'u') autoFont(text);   // switch CJK glyph font to match the reply
    wrapAndAdd(text, type);
    _scroll = 0;
    _dirtyLog = true;
}

void DianaUI::setInput(const String& text) {
    _input = text;
    _dirtyIn = true;
}

void DianaUI::scroll(int lines) {
    int rows = LOG_H / _lineH;
    int maxScroll = (int)_lines.size() - rows;
    if (maxScroll < 0) maxScroll = 0;
    _scroll = constrain(_scroll + lines, 0, maxScroll);
    _dirtyLog = true;
}

void DianaUI::setStatus(int battery, bool charging, int rssi, bool muted, bool sd) {
    if (_battery != battery || _charging != charging || _rssi != rssi || _muted != muted || _sd != sd) {
        _battery = battery; _charging = charging; _rssi = rssi; _muted = muted; _sd = sd;
        _dirtyBar = true;
    }
}

String DianaUI::clockText() {
    time_t now = time(nullptr);
    if (now < 1600000000) return "--:--";
    struct tm t;
    localtime_r(&now, &t);
    char buf[8];
    strftime(buf, sizeof(buf), "%H:%M", &t);
    return String(buf);
}

// ─────────────────────────── drawing ────────────────────────────────────
void DianaUI::drawLog() {
    _log.fillSprite((uint32_t)C_BLACK);
    int rows = LOG_H / _lineH;
    int total = (int)_lines.size();
    int end = total - _scroll;
    int startIdx = end - rows;
    if (startIdx < 0) startIdx = 0;
    int y = LOG_H - (end - startIdx) * _lineH;
    if (y < 0) y = 0;
    _log.setTextDatum(top_left);
    for (int i = startIdx; i < end; ++i) {
        const Line& L = _lines[i];
        _log.setTextColor(colorFor(L.type), (uint32_t)C_BLACK);
        _log.drawString(L.text, 2, y);
        y += _lineH;
    }
    if (_scroll > 0) {
        _log.fillRect(SCREEN_W - 3, 0, 3, LOG_H, (uint32_t)C_CYAN_DK);
        int h = max(4, LOG_H * rows / max(total, 1));
        int pos = (LOG_H - h) * (total - rows - _scroll) / max(total - rows, 1);
        _log.fillRect(SCREEN_W - 3, pos, 3, h, (uint32_t)C_CYAN_DIM);
    }
    _log.pushSprite(0, LOG_Y);
}

void DianaUI::drawBar() {
    _bar.fillSprite((uint32_t)C_BG);
    _bar.drawFastHLine(0, BAR_H - 1, SCREEN_W, (uint32_t)C_CYAN_DK);
    drawCore(_bar, 6, BAR_H / 2 - 1, _phase, 4);
    _bar.setTextDatum(middle_left);
    _bar.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BG);
    _bar.drawString("DIANA", 14, BAR_H / 2 - 1);
    uint32_t sc = C_WHITE;
    if (_state == DianaState::RECORDING) sc = C_RED;
    else if (_state == DianaState::THINKING) sc = C_AMBER;
    else if (_state == DianaState::SPEAKING) sc = C_GREEN;
    else if (_state == DianaState::STANDBY) sc = C_DGREY;
    _bar.setTextColor(sc, (uint32_t)C_BG);
    _bar.drawString(stateLabel(), 50, BAR_H / 2 - 1);

    // right side: mute, sd, wifi, battery, clock
    int x = SCREEN_W - 2;
    _bar.setTextDatum(middle_right);
    _bar.setTextColor((uint32_t)C_GREY, (uint32_t)C_BG);
    String clk = clockText();
    _bar.drawString(clk, x, BAR_H / 2 - 1);
    x -= _bar.textWidth(clk) + 5;
    if (_battery >= 0) {
        uint32_t bc = _battery < 20 ? C_RED : (_charging ? C_GREEN : C_GREY);
        String b = String(_battery) + "%" + (_charging ? "+" : "");
        _bar.setTextColor(bc, (uint32_t)C_BG);
        _bar.drawString(b, x, BAR_H / 2 - 1);
        x -= _bar.textWidth(b) + 5;
    }
    // wifi bars
    int bars = _rssi == 0 ? 0 : (_rssi > -55 ? 4 : _rssi > -65 ? 3 : _rssi > -75 ? 2 : 1);
    for (int i = 0; i < 4; ++i) {
        int h = 2 + i * 2;
        _bar.fillRect(x - 10 + i * 3, BAR_H - 3 - h, 2, h, i < bars ? (uint32_t)C_CYAN : (uint32_t)C_DGREY);
    }
    x -= 15;
    if (_muted) { _bar.setTextColor((uint32_t)C_RED, (uint32_t)C_BG); _bar.drawString("M", x, BAR_H / 2 - 1); x -= 9; }
    if (!_sd)   { _bar.setTextColor((uint32_t)C_AMBER, (uint32_t)C_BG); _bar.drawString("noSD", x, BAR_H / 2 - 1); }
    _bar.pushSprite(0, 0);
}

void DianaUI::drawInput() {
    _in.fillSprite((uint32_t)C_BLACK);
    _in.drawFastHLine(0, 0, SCREEN_W, (uint32_t)C_CYAN_DK);
    _in.setTextDatum(middle_left);
    int cy = INPUT_H / 2 + 1;
    bool blink = (millis() / 500) & 1;
    if (_state == DianaState::RECORDING) {
        _in.setTextColor((uint32_t)C_RED, (uint32_t)C_BLACK);
        _in.drawString(blink ? "REC" : "   ", 3, cy);
        _in.fillCircle(26, cy, 3, blink ? (uint32_t)C_RED : (uint32_t)C_DGREY);
        int w = constrain(_level / 40, 0, 120);
        _in.drawRect(34, cy - 4, 122, 9, (uint32_t)C_CYAN_DK);
        _in.fillRect(35, cy - 3, w, 7, w > 100 ? (uint32_t)C_RED : (uint32_t)C_CYAN);
        _in.setTextColor((uint32_t)C_GREY, (uint32_t)C_BLACK);
        _in.drawString("TAB=send", 162, cy);
    } else if (_state == DianaState::THINKING) {
        _in.setTextColor((uint32_t)C_AMBER, (uint32_t)C_BLACK);
        int dots = (millis() / 300) % 4;
        String s = "neural link";
        for (int i = 0; i < dots; ++i) s += '.';
        _in.drawString(s, 3, cy);
    } else if (_state == DianaState::SPEAKING) {
        _in.setTextColor((uint32_t)C_GREEN, (uint32_t)C_BLACK);
        _in.drawString("speaking", 3, cy);
        // little equalizer
        for (int i = 0; i < 12; ++i) {
            int h = 2 + (int)((sinf(_phase * 2 + i) + 1.0f) * 3.5f);
            _in.fillRect(70 + i * 5, cy + 4 - h, 3, h, (uint32_t)C_GREEN);
        }
        _in.setTextColor((uint32_t)C_GREY, (uint32_t)C_BLACK);
        _in.drawString("ESC=stop", 162, cy);
    } else {
        _in.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BLACK);
        _in.drawString(">", 3, cy);
        _in.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BLACK);
        // show the tail of long input
        String shown = _input;
        int maxW = SCREEN_W - 20;
        while (shown.length() && _in.textWidth(shown) > maxW) {
            size_t cl = utf8Len((unsigned char)shown[0]);
            shown = shown.substring(cl);
        }
        _in.drawString(shown, 12, cy);
        int cx = 12 + _in.textWidth(shown) + 1;
        if (blink) _in.fillRect(cx, cy - 4, 5, 9, (uint32_t)C_CYAN);
        if (_input.isEmpty() && _state == DianaState::IDLE) {
            if (_listeningHint) {
                // small "ear open" indicator + live level bar on the right
                uint32_t lc = (millis() / 500) & 1 ? (uint32_t)C_CYAN : (uint32_t)C_CYAN_DIM;
                _in.setTextColor(lc, (uint32_t)C_BLACK);
                _in.drawString("listening", 20, cy);
                int w = constrain(_level / 24, 0, 80);
                _in.drawRect(SCREEN_W - 86, cy - 4, 82, 9, (uint32_t)C_CYAN_DK);
                _in.fillRect(SCREEN_W - 85, cy - 3, w, 7, w > 66 ? (uint32_t)C_GREEN : (uint32_t)C_CYAN_DIM);
            } else {
                _in.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
                _in.drawString("say 'diana' or TAB", 20, cy);
            }
        }
    }
    _in.pushSprite(0, INPUT_Y);
}

void DianaUI::redraw() {
    if (_scene != Scene::HUD) return;
    if (_dirtyBar) { drawBar(); _dirtyBar = false; }
    if (_dirtyLog) { drawLog(); _dirtyLog = false; }
    if (_dirtyIn)  { drawInput(); _dirtyIn = false; }
}

void DianaUI::tick() {
    uint32_t now = millis();
    if (now - _lastAnim < 50) return;
    _lastAnim = now;
    _phase += 0.18f;
    if (_phase > 6.283f) _phase -= 6.283f;
    static M5Canvas ring(&M5.Display);              // standby pulse; 3.9 KB, freed when the scene changes
    if (_scene != Scene::STANDBY && ring.width()) ring.deleteSprite();
    if (_scene == Scene::STANDBY) {
        // pulsing ring under the title, redrawn in place
        if (!ring.width()) { ring.setColorDepth(16); ring.createSprite(44, 44); }
        ring.fillSprite((uint32_t)C_BLACK);
        float p = (sinf(_phase) + 1.0f) * 0.5f;
        ring.drawCircle(22, 22, 20, (uint32_t)C_CYAN_DK);
        ring.drawCircle(22, 22, 8 + (int)(p * 10), (uint32_t)C_CYAN_DIM);
        ring.fillCircle(22, 22, 2 + (int)(p * 4), (uint32_t)C_CYAN);
        ring.pushSprite(SCREEN_W / 2 - 22, 84 - 8);
        return;
    }
    if (_scene != Scene::HUD) return;
    // animate the input strip while busy, the core pulse every 150 ms
    static uint32_t lastBlink = 0;
    if (_state == DianaState::RECORDING || _state == DianaState::THINKING || _state == DianaState::SPEAKING) _dirtyIn = true;
    else if ((now / 500) != (lastBlink / 500)) _dirtyIn = true;   // cursor blink
    lastBlink = now;
    if (now - _lastBar > 150) { _lastBar = now; _dirtyBar = true; }
    redraw();
}
