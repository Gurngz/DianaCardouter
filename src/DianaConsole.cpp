// Serial debug console: drive Diana over USB (wake, talk, /commands) and - with
// DIANA_DEBUG_CONSOLE - probe the ES8311 codec registers live to bisect audio noise.
#include <M5Cardputer.h>
#include <WiFi.h>
#include "config.h"
#include "DianaApp.h"
#include "DianaConfig.h"
#include "DianaUI.h"
#include "DianaAudio.h"
#include "DianaForth.h"

static bool g_freezeUI = false;       // !ui off: skip all display redraws (isolate screen-coupling from mic)
bool consoleUiFrozen() { return g_freezeUI; }

// ── serial debug console ──────────────────────────────────────────────────────
// Lets me drive Diana over USB for a remote check-up: wake her, talk, and probe /
// toggle the ES8311 audio registers live while the user listens for the "helicopter"
// chop, so we can bisect its source without a reflash per guess.
#if DIANA_DEBUG_CONSOLE
static int      g_hunt = -1;              // audio-source hunt step (-1 = idle)
static uint32_t g_huntAt = 0;

static void es8311Dump() {
    const uint8_t regs[] = {0x00,0x01,0x02,0x0D,0x0E,0x12,0x13,0x14,0x17,0x1C,0x31,0x32,0x37,0x45};
    Serial.print("[ES8311]");
    for (uint8_t r : regs) Serial.printf(" %02X=%02X", r, M5.In_I2C.readRegister8(0x18, r, 100000));
    Serial.println();
}

// Additive source-elimination while she LISTENS: each step silences one more suspect,
// keeping the earlier ones off. The user reports the FIRST STEP where the chop stops ->
// that names the source. Ordered least-to-most disruptive.
static void huntStep(int step) {
    if (step <= 1 && !Audio.isListening()) Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs);
    switch (step) {
        case 0: Serial.println("[HUNT] STEP0 baseline listening (chop should be present)"); break;
        case 1: Serial.println("[HUNT] STEP1 stop the MIC (M5.Mic.end) -> tests mic I2S/DMA activity");
                if (M5.Mic.isRunning()) M5.Mic.end(); break;
        case 2: Serial.println("[HUNT] STEP2 full codec analog power-down 0x0D=0xFC + CSM off -> tests ES8311 analog/amp");
                M5.In_I2C.writeRegister8(0x18,0x32,0x00,100000);
                M5.In_I2C.writeRegister8(0x18,0x13,0x00,100000);
                M5.In_I2C.writeRegister8(0x18,0x12,0x02,100000);
                M5.In_I2C.writeRegister8(0x18,0x0D,0xFC,100000);
                M5.In_I2C.writeRegister8(0x18,0x00,0x00,100000); break;
        case 3: Serial.println("[HUNT] STEP3 blank the screen (display sleep + backlight off) -> tests display SPI/backlight");
                M5.Display.setBrightness(0); M5.Display.sleep(); break;
        case 4: Serial.println("[HUNT] STEP4 WiFi modem-sleep OFF + CPU 240MHz -> tests WiFi power-save beacon ticks");
                WiFi.setSleep(false); setCpuFrequencyMhz(240); break;
        case 5: Serial.println("[HUNT] STEP5 WiFi radio OFF -> tests WiFi RF/power bursts (the classic 'helicopter')");
                WiFi.mode(WIFI_OFF); break;
        default:
                Serial.println("[HUNT] END. If it never went quiet even here, it's a DC-DC/rail whine independent of firmware.");
                Serial.println("[HUNT] reboot to restore normal operation (WiFi/screen were turned off for the test).");
                g_hunt = -1; return;
    }
}
#endif

static void consoleLine(String line) {
    line.trim();
    if (line.isEmpty()) return;
    // echo, but never the secrets in /key and /wifi lines
    if (line.startsWith("/key") || line.startsWith("/addkey") || line.startsWith("/wifi")) {
        int sp = line.indexOf(' ');
        Serial.printf("[CONSOLE] > %s ****\n", (sp < 0 ? line : line.substring(0, sp)).c_str());
    } else {
        Serial.printf("[CONSOLE] > %s\n", line.c_str());
    }
    if (line.startsWith("/")) { handleCommand(line); return; }
    // Forth REPL: `forth` enters, `bye` leaves; everything between is evaluated verbatim.
    static bool forthRepl = false;
    if (forthRepl) {
        if (line == "bye") { forthRepl = false; Serial.println("[FORTH] bye"); return; }
        int errs; String out = Forth.eval(line, errs);
        if (out.length()) Serial.println(out);
        Serial.println(errs ? "? error" : "ok");
        return;
    }
    if (line == "forth") { forthRepl = true; Serial.println("[FORTH] REPL - type Forth, 'diana-words' for the vocabulary, 'bye' to leave"); return; }
#if DIANA_DEBUG_CONSOLE
    if (line.startsWith("!")) {
        String c = line.substring(1); c.trim();
        String a; int sp = c.indexOf(' ');
        if (sp >= 0) { a = c.substring(sp + 1); a.trim(); c = c.substring(0, sp); }
        c.toLowerCase();
        if (c == "help") {
            Serial.println("audio: !regs !rd XX !wr XX YY !dac on|off !hp on|off !mute on|off !ana XX");
            Serial.println("drive: !state !wake !sleep !listen !stoplisten !chirp | !hunt / !hunt stop");
            Serial.println("or type '/command' or plain text to talk to her");
        }
        else if (c == "state") Serial.printf("[STATE] awake=%d listening=%d capturing=%d uiState=%d heap=%u micRun=%d spkRun=%d\n",
                                             awake, Audio.isListening(), Audio.capturing(), (int)UI.state(),
                                             (unsigned)ESP.getFreeHeap(), M5.Mic.isRunning(), M5.Speaker.isRunning());
        else if (c == "regs") es8311Dump();
        else if (c == "rd") { uint8_t r = strtoul(a.c_str(), nullptr, 16); Serial.printf("[RD] %02X=%02X\n", r, M5.In_I2C.readRegister8(0x18, r, 100000)); }
        else if (c == "wr") { int s2 = a.indexOf(' '); uint8_t r = strtoul(a.substring(0, s2).c_str(), nullptr, 16); uint8_t v = strtoul(a.substring(s2 + 1).c_str(), nullptr, 16); M5.In_I2C.writeRegister8(0x18, r, v, 100000); Serial.printf("[WR] %02X<=%02X\n", r, v); }
        else if (c == "dac") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x12, on ? 0x00 : 0x02, 100000); Serial.printf("[DAC] %s\n", on ? "up" : "down"); }
        else if (c == "hp") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x13, on ? 0x10 : 0x00, 100000); Serial.printf("[HP] %s\n", on ? "on" : "off"); }
        else if (c == "mute") { bool on = (a == "on"); M5.In_I2C.writeRegister8(0x18, 0x32, on ? 0x00 : 0xBF, 100000); Serial.printf("[DACvol] %s\n", on ? "muted" : "0dB"); }
        else if (c == "ana") { uint8_t v = strtoul(a.c_str(), nullptr, 16); M5.In_I2C.writeRegister8(0x18, 0x0D, v, 100000); Serial.printf("[ANA] 0D<=%02X\n", v); }
        else if (c == "wake") { if (!awake) { UI.showHud(); welcomeProtocol(); } else Serial.println("already awake"); }
        else if (c == "sleep") enterStandby();
        else if (c == "listen") { if (!Audio.isListening()) Audio.startListening(REC_PATH, Config.vadThreshold, Config.silenceMs); Serial.println("[LISTEN] on"); }
        else if (c == "stoplisten") { Audio.stopListening(); Serial.println("[LISTEN] off"); }
        else if (c == "chirp") Audio.chirpWake();
        else if (c == "tone") {   // full speaker I2S re-init + raw tone, report the speaker task state
            if (M5.Mic.isRunning()) M5.Mic.end();
            M5.Speaker.end(); delay(40);
            bool b = M5.Speaker.begin(); M5.Speaker.setVolume(220);
            M5.Speaker.tone(1000, 700, 0, true);
            delay(60);
            Serial.printf("[TONE] begin=%d running=%d playing=%d\n", b, M5.Speaker.isRunning(), M5.Speaker.isPlaying(0));
        }
        else if (c == "pause") { Audio.stopListening(); Serial.println("[PAUSE] mic OFF (hands-free restarts it on the next loop). Is the sound GONE now?"); }
        else if (c == "ui") { g_freezeUI = (a == "off"); Serial.printf("[UI] redraws %s (mic unchanged). Sound %s?\n", g_freezeUI ? "FROZEN" : "live", g_freezeUI ? "gone" : "back"); }
        else if (c == "hunt") { if (a == "stop") { g_hunt = -1; Serial.println("[HUNT] stopped"); } else { g_hunt = 0; g_huntAt = millis() - 4000; Serial.println("[HUNT] starting; tell me the STEP number where the chop stops"); } }
        else Serial.println("[CONSOLE] unknown ! command (try !help)");
        return;
    }
#endif
    String lower = line; lower.toLowerCase();
    if (!awake) {
        if (isWakePhrase(lower)) { UI.showHud(); welcomeProtocol(); }
        else Serial.println("[CONSOLE] she's asleep - send 'wake up diana' first");
        return;
    }
    // A serial turn must not start while the mic owns the I2S bus (the turn switches to the speaker).
    if (UI.state() == DianaState::RECORDING) { Audio.stopRecording(); UI.setState(DianaState::IDLE); UI.log("(cancelled)", 's'); }
    if (Audio.isListening()) { Audio.stopListening(); UI.setListening(false, 0); }
    UI.log(line, 'u'); appendChatLog('u', line); g_lastUserMsg = line;
    runTurn(line, nullptr, 0);
}

void serviceSerialConsole() {
    static String buf;
    while (Serial.available()) {
        char ch = (char)Serial.read();
        if (ch == '\r') continue;
        if (ch == '\n') { consoleLine(buf); buf = ""; }
        else if (buf.length() < 220) buf += ch;
    }
#if DIANA_DEBUG_CONSOLE
    if (g_hunt >= 0 && millis() - g_huntAt > 4000) { g_huntAt = millis(); huntStep(g_hunt++); }
#endif
}

