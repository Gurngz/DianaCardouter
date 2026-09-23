#include "DianaMusic.h"
#include "DianaUI.h"
#include "config.h"
#include <M5Cardputer.h>

DianaMusic Music;

// note frequencies (Hz); 0 = rest
#define R 0
#define C4 262
#define D4 294
#define E4 330
#define F4 349
#define G4 392
#define A4 440
#define B4 494
#define C5 523
#define D5 587
#define E5 659
#define F5 698
#define G5 784
#define A5 880
#define B5 988
#define C6 1047

struct Note { uint16_t f; uint16_t ms; };

// ── original / public-domain chiptune loops ───────────────────────────────
static const Note T_arcade[] = {
    {C5,110},{E5,110},{G5,110},{C6,160},{G5,110},{E5,110},{C5,110},{R,80},
    {D5,110},{F5,110},{A5,110},{D5,160},{A5,110},{F5,110},{D5,110},{R,80},
    {E5,110},{G5,110},{B5,110},{E5,160},{B5,110},{G5,110},{E5,110},{R,120},
};
static const Note T_starlight[] = {
    {C5,260},{E5,260},{G5,260},{E5,260},{A4,260},{C5,260},{E5,260},{C5,260},
    {F4,260},{A4,260},{C5,260},{A4,260},{G4,260},{B4,260},{D5,260},{G4,300},{R,160},
};
static const Note T_blocks[] = {
    {C4,120},{C5,120},{G4,120},{C5,120},{A4,120},{C5,120},{G4,120},{C5,120},
    {F4,120},{C5,120},{G4,120},{C5,120},{E4,120},{G4,120},{C5,200},{R,100},
};
static const Note T_neon[] = {
    {E5,90},{E5,90},{R,60},{E5,90},{R,60},{C5,90},{E5,90},{G5,140},{R,120},{G4,140},{R,140},
    {C5,120},{G4,120},{E4,120},{A4,120},{B4,120},{A4,120},{G4,120},{E5,120},{G5,120},{A5,160},{R,120},
};
static const Note T_joy[] = {   // Ode to Joy (Beethoven, public domain)
    {E5,220},{E5,220},{F5,220},{G5,220},{G5,220},{F5,220},{E5,220},{D5,220},
    {C5,220},{C5,220},{D5,220},{E5,220},{E5,330},{D5,110},{D5,440},
    {E5,220},{E5,220},{F5,220},{G5,220},{G5,220},{F5,220},{E5,220},{D5,220},
    {C5,220},{C5,220},{D5,220},{E5,220},{D5,330},{C5,110},{C5,440},{R,200},
};

struct Track { const char* name; const Note* notes; int len; };
static const Track TRACKS[] = {
    {"Arcade",    T_arcade,    sizeof(T_arcade) / sizeof(Note)},
    {"Starlight", T_starlight, sizeof(T_starlight) / sizeof(Note)},
    {"Blocks",    T_blocks,    sizeof(T_blocks) / sizeof(Note)},
    {"Neon Run",  T_neon,      sizeof(T_neon) / sizeof(Note)},
    {"Ode to Joy",T_joy,       sizeof(T_joy) / sizeof(Note)},
};
static const int NTRACKS = sizeof(TRACKS) / sizeof(Track);
static const int MUS_CH = 3;    // dedicated speaker channel (0=TTS,1=chirps)

int DianaMusic::trackCount() const { return NTRACKS; }
String DianaMusic::trackName(int i) const { return (i >= 0 && i < NTRACKS) ? String(TRACKS[i].name) : String("?"); }

void DianaMusic::startNote() {
    const Track& t = TRACKS[_track];
    if (_note >= t.len) _note = 0;              // loop
    const Note& n = t.notes[_note];
    _curDur = n.ms;
    _noteStart = millis();
    if (M5.Mic.isRunning()) M5.Mic.end();
    if (!M5.Speaker.isRunning()) M5.Speaker.begin();
    if (n.f > 0) { M5.Speaker.tone((float)n.f, n.ms + 20, MUS_CH, true); _lastFreq = n.f; }
    else { M5.Speaker.stop(MUS_CH); _lastFreq = 0; }
}

String DianaMusic::play(int track) {
    if (track >= 0 && track < NTRACKS) _track = track;
    if (_track < 0 || _track >= NTRACKS) _track = 0;
    _note = 0;
    _playing = true;
    startNote();
    _dirty = true;
    return String("Playing '") + TRACKS[_track].name + "'.";
}

String DianaMusic::playByName(const String& name) {
    String q = name; q.toLowerCase(); q.trim();
    for (int i = 0; i < NTRACKS; ++i) {
        String tn = TRACKS[i].name; tn.toLowerCase();
        if (tn.indexOf(q) >= 0 || q.indexOf(tn) >= 0) return play(i);
    }
    if (q.isEmpty()) return play(_track);
    return "No track called '" + name + "'. I have: " + list();
}

String DianaMusic::stop() {
    _playing = false;
    M5.Speaker.stop(MUS_CH);
    _dirty = true;
    return "Music stopped.";
}

String DianaMusic::next() {
    _track = (_track + 1) % NTRACKS;
    return play(_track);
}

String DianaMusic::list() {
    String s;
    for (int i = 0; i < NTRACKS; ++i) { if (i) s += ", "; s += TRACKS[i].name; }
    return s;
}

void DianaMusic::open(int track) {
    _open = true;
    play(track);
    drawScreen(true);
}

void DianaMusic::close() {
    stop();
    _open = false;
}

void DianaMusic::handleKeysRaw() {
    if (!(M5Cardputer.Keyboard.isChange() && M5Cardputer.Keyboard.isPressed())) return;
    auto st = M5Cardputer.Keyboard.keysState();
    bool left = st.left, right = st.right, esc = st.esc;
    for (auto c : st.word) { if (c == ',') left = true; else if (c == '/') right = true; else if (c == '`') esc = true; }
    if (esc) { close(); return; }
    if (left)  { _track = (_track - 1 + NTRACKS) % NTRACKS; play(_track); }
    if (right) { next(); }
    if (st.enter || st.space) { if (_playing) stop(); else play(_track); }
    _dirty = true;
}

void DianaMusic::service() {
    if (!_open && !_playing) return;
    // advance the sequence
    if (_playing && millis() - _noteStart >= _curDur) {
        _note++;
        startNote();
    }
    if (_open && millis() - _lastDraw > 60) { _lastDraw = millis(); drawScreen(false); }
}

// ── pixel visualizer ──────────────────────────────────────────────────────
void DianaMusic::drawScreen(bool full) {
    auto& d = M5.Display;
    if (full) { d.fillScreen((uint32_t)C_BLACK); }
    // header
    d.fillRect(0, 0, SCREEN_W, 14, (uint32_t)C_BG);
    d.setTextDatum(top_left);
    d.setFont(&fonts::Font0);
    d.setTextColor((uint32_t)C_CYAN, (uint32_t)C_BG);
    d.drawString("PIXEL MUSIC", 4, 3);
    d.setTextDatum(top_right);
    d.setTextColor(_playing ? (uint32_t)C_GREEN : (uint32_t)C_DGREY, (uint32_t)C_BG);
    d.drawString(_playing ? "PLAY" : "PAUSE", SCREEN_W - 4, 3);
    d.setTextDatum(top_center);
    d.setTextColor((uint32_t)C_WHITE, (uint32_t)C_BG);
    d.drawString(TRACKS[_track].name, SCREEN_W / 2, 3);

    // pixel bar grid
    const int cols = 16, rows = 8;
    const int cw = SCREEN_W / cols, ch = 9;
    const int gy = 20;
    _phase += 0.35f;
    int pitch = _lastFreq ? _lastFreq : 300;
    for (int x = 0; x < cols; ++x) {
        // bar height from pitch + a travelling wave, only lively while playing
        float w = _playing ? (sinf(_phase + x * 0.6f) * 0.5f + 0.5f) : 0.15f;
        float pf = (pitch - 250) / 800.0f; if (pf < 0) pf = 0; if (pf > 1) pf = 1;
        int h = 1 + (int)((0.35f + 0.65f * w) * pf * rows);
        if (h > rows) h = rows;
        for (int y = 0; y < rows; ++y) {
            bool on = y < h;
            uint32_t col = !on ? (uint32_t)0x001015u
                         : (y >= rows - 2 ? (uint32_t)C_RED
                         : y >= rows - 4 ? (uint32_t)C_AMBER : (uint32_t)C_CYAN);
            int px = x * cw + 1;
            int py = gy + (rows - 1 - y) * ch;
            d.fillRect(px, py, cw - 2, ch - 2, col);
        }
    }
    // footer
    d.fillRect(0, SCREEN_H - 12, SCREEN_W, 12, (uint32_t)C_BLACK);
    d.setTextDatum(top_left);
    d.setTextColor((uint32_t)C_DGREY, (uint32_t)C_BLACK);
    d.drawString(", / track  SPACE play  ` exit", 3, SCREEN_H - 10);
}
