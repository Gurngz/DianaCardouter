#include "DianaForth.h"
#include <SD.h>
#include <M5Cardputer.h>
#undef MAX_INPUT                       // Arduino defines it; the engine has its own
#include "forth_core.h"
#include "config.h"
#include "DianaApp.h"
#include "DianaTune.h"
#include "DianaConfig.h"
#include "DianaUI.h"
#include "DianaAudio.h"
#include "DianaNet.h"
#include "DianaIR.h"

DianaForth Forth;
DianaTune  Tune;

#ifndef FORTH_HEAP_BYTES
#define FORTH_HEAP_BYTES (8 * 1024)    // `variable` / `allot` space; the dictionary itself is static
#endif

// ── engine I/O: everything a program prints is captured here ─────────────────
static String s_out;
static int  io_get() { return -1; }
static void io_put(int c) { if (s_out.length() < 2000) s_out += (char)c; }

// ── stack helpers ─────────────────────────────────────────────────────────────
static String popStr() {                       // ( c-addr u -- )
    intptr_t u = forth_pop();
    const char* s = (const char*)forth_pop();
    if (!s || u <= 0) return String();
    return String(s, (unsigned)u);
}
static void print(const String& s) { for (size_t i = 0; i < s.length(); ++i) io_put(s[i]); }
static void println(const String& s) { print(s); io_put('\n'); }

// ── tunables: one table for config fields and DianaTune ──────────────────────
struct TuneEntry { const char* name; int* ptr; int lo, hi; void (*apply)(int); const char* help; };
static void applyVolume(int v)     { Audio.setVolume(v); }
static void applyBrightness(int v) { M5.Display.setBrightness(v); }
static const TuneEntry TUNES[] = {
    { "vad",           &Config.vadThreshold,    100, 4000, nullptr,          "RMS level that counts as speech" },
    { "silence_ms",    &Config.silenceMs,       200, 5000, nullptr,          "silence after speech that ends a recording" },
    { "mic_gain",      &Config.micGain,           8,  128, nullptr,          "ES8311 mic gain (reboot to apply)" },
    { "volume",        &Config.volume,            0,  255, applyVolume,      "speaker volume" },
    { "brightness",    &Config.brightness,        8,  255, applyBrightness,  "screen backlight" },
    { "idle_sleep",    &Config.idleSleepSec,      0, 3600, nullptr,          "(stored; auto-sleep not implemented)" },
    { "stream_chunk",  &Tune.streamChunkSamples, 480, 8000, nullptr,         "TTS buffer samples @24k (next reply)" },
    { "stream_prebuf", &Tune.streamPrebuffer,     1,    3, nullptr,          "buffers held before playback starts" },
    { "reply_tokens",  &Tune.replyMaxTokens,     32, 2048, nullptr,          "Gemini maxOutputTokens" },
    { "rec_max_sec",   &Tune.recMaxSeconds,       2,   60, nullptr,          "longest recording / utterance" },
    { "history_turns", &Tune.historyMaxTurns,     1,   32, nullptr,          "exchanges kept in context" },
    { "history_chars", &Tune.historyMaxChars,   500, 12000, nullptr,         "context size cap" },
    { "spool",         &Tune.spool,               0,    1, nullptr,          "1 = SD jitter buffer for voice replies" },
    { "jitter_ms",     &Tune.jitterMarginMs,      0, 3000, nullptr,          "extra head start before a reply plays" },
    { "jitter_max_ms", &Tune.jitterMaxMs,         0, 15000, nullptr,         "longest wait before the first word (0 = no cap)" },
    { "speech_cps",    &Tune.speechCps,           5,   30, nullptr,          "chars/sec used to estimate reply length" },
    { "codec_hold",    &Tune.codecHold,           0,    1, nullptr,          "1 = no codec power-down between mic and speaker (anti-pop)" },
};
static const TuneEntry* findTune(const String& name) {
    for (auto& t : TUNES) if (name.equalsIgnoreCase(t.name)) return &t;
    println("? no tunable '" + name + "' (type tunes)");
    return nullptr;
}

// ── Diana words ───────────────────────────────────────────────────────────────
// Anything that plays audio must release the hands-free mic first (shared I2S, same as a real turn).
static void releaseMic() { if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); } }
static void w_say()    { String s = popStr(); releaseMic(); speakText(s); }                     // ( addr u -- )
static void w_ask()    { String s = popStr(); releaseMic(); UI.log(s, 'u'); appendChatLog('u', s); g_lastUserMsg = s; runTurn(s, nullptr, 0); }
static void w_log()    { UI.log(popStr(), 's'); }
static void w_set()    { String v = popStr(); String n = popStr(); println(applyDeviceSetting(n, v)); }   // ( name val -- )
static void w_tuneGet(){ const TuneEntry* t = findTune(popStr()); forth_push(t ? *t->ptr : 0); }         // ( name -- n )
static void w_tuneSet(){                                                                                 // ( name n -- )
    int v = (int)forth_pop(); String n = popStr();
    const TuneEntry* t = findTune(n); if (!t) return;
    if (v < t->lo) v = t->lo; if (v > t->hi) v = t->hi;
    *t->ptr = v; if (t->apply) t->apply(v);
    println(String(t->name) + " = " + v);
}
static void w_tunes()  { for (auto& t : TUNES) println(String(t.name) + " = " + *t.ptr + "  (" + t.lo + ".." + t.hi + ") " + t.help); }
static void w_cfgSave(){ println(Config.save(sdOk) ? "config saved" : "config save failed"); }
static void w_timer()  { String l = popStr(); int s = (int)forth_pop(); println(addTimer(s, l)); }      // ( secs label -- )
static void w_ir()     { String p = popStr(); uint32_t c = (uint32_t)forth_pop(); uint32_t a = (uint32_t)forth_pop(); println(dianaIrSend(p, a, c)); }  // ( addr cmd proto -- )
static void w_irRun()  { println(dianaIrRunNamed(popStr())); }                                        // ( name -- )
static void w_tone()   { int ms = (int)forth_pop(); int f = (int)forth_pop(); releaseMic(); Audio.toneMs((float)f, (uint32_t)ms); }   // ( hz ms -- )
static void w_beep()   { releaseMic(); Audio.chirpAck(); }
static void w_mute()   { muted = forth_pop() != 0; refreshStatusBar(); }                              // ( flag -- )
static void w_sleep()  { enterStandby(); }
static void w_wake()   { if (!awake) { UI.showHud(); welcomeProtocol(); } }
static void w_status() { println(deviceStatusText()); }
static void w_heap()   { forth_push((intptr_t)ESP.getFreeHeap()); }
static void w_ms()     { forth_push((intptr_t)millis()); }
static void w_wait()   { int ms = (int)forth_pop(); if (ms > 0 && ms <= 10000) delay(ms); }           // ( ms -- )
static void w_wifiQ()  { forth_push(Net.isConnected() ? -1 : 0); }
static void w_sdQ()    { forth_push(sdOk ? -1 : 0); }
static void w_awakeQ() { forth_push(awake ? -1 : 0); }
static void w_include(){ String p = popStr(); if (!p.startsWith("/")) p = String(DIANA_DIR) + "/" + p; if (!p.endsWith(".fs")) p += ".fs"; int n = Forth.runFile(p.c_str(), false); println(String("ran ") + n + " lines from " + p); }
static void w_words()  {
    println("say ask log ( addr u -- )   set ( name val -- )   tune@ ( name -- n )   tune! ( name n -- )   tunes   cfg-save");
    println("timer ( secs label -- )   ir ( addr cmd proto -- )   ir-run ( name -- )   tone ( hz ms -- )   beep   mute ( f -- )");
    println("sleep wake status   heap ms ( -- n )   wait ( ms -- )   wifi? sd? awake? ( -- f )   include ( name -- )   diana-words");
}

void DianaForth::registerWords() {
    struct { const char* n; forth_word_fn f; } W[] = {
        {"say", w_say}, {"ask", w_ask}, {"log", w_log}, {"set", w_set},
        {"tune@", w_tuneGet}, {"tune!", w_tuneSet}, {"tunes", w_tunes}, {"cfg-save", w_cfgSave},
        {"timer", w_timer}, {"ir", w_ir}, {"ir-run", w_irRun}, {"tone", w_tone}, {"beep", w_beep}, {"mute", w_mute},
        {"sleep", w_sleep}, {"wake", w_wake}, {"status", w_status}, {"heap", w_heap}, {"ms", w_ms}, {"wait", w_wait},
        {"wifi?", w_wifiQ}, {"sd?", w_sdQ}, {"awake?", w_awakeQ}, {"include", w_include}, {"diana-words", w_words},
    };
    for (auto& w : W) forth_register_word(w.n, w.f);
}

bool DianaForth::begin() {
    if (_ready) return true;
    if (forth_init(FORTH_HEAP_BYTES) != 0) { Serial.println("[FORTH] init failed (heap)"); return false; }
    forth_set_io(io_get, io_put);
    registerWords();
    _ready = true;
    Serial.printf("[FORTH] engine up, %d B heap, %d B free\n", FORTH_HEAP_BYTES, forth_heap_free());
    return true;
}

String DianaForth::eval(const String& line, int& errors) {
    errors = 0;
    if (!_ready) return "forth: not running";
    int before = forth_error_count();
    String outer = s_out;                  // `include` re-enters eval(); keep the caller's output
    s_out = "";
    forth_eval(line.c_str());
    errors = forth_error_count() - before;
    String out = s_out; s_out = outer;
    out.trim();
    return out;
}

int DianaForth::runFile(const char* path, bool logOutput) {
    if (!_ready || !sdOk) return 0;
    File f = SD.open(path, FILE_READ);
    if (!f) return 0;
    int lines = 0, errs = 0;
    while (f.available()) {
        String line = f.readStringUntil('\n');
        line.trim();
        if (line.isEmpty() || line.startsWith("\\")) { continue; }
        if (line.length() > 250) { UI.log(String(path) + ": line too long (max 250)", 'w'); continue; }
        int e; String out = eval(line, e);
        lines++; errs += e;
        if (out.length() && (logOutput || e)) UI.log(out, e ? 'w' : 's');
        if (e) UI.log(String(path) + " line " + lines + ": " + line, 'w');
    }
    f.close();
    Serial.printf("[FORTH] %s: %d lines, %d errors, heap free %d\n", path, lines, errs, forth_heap_free());
    return lines;
}
