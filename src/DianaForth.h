// ESPIDFORTH bridge: a small Forth interpreter with a Diana vocabulary, for tuning the
// device and scripting behaviour from the SD card without a reflash.
//   /forth <code>      evaluate one line (tunables: s" vad" 700 tune!) on the HUD (or serial console)
//   /fs <name>         run /diana/<name>.fs from the SD card
//   forth ... bye      REPL mode on the serial console
//   /diana/boot.fs     runs at boot, after config and audio are up
// Vocabulary: type `words` (engine) or `diana-words` (this bridge).
#pragma once
#include <Arduino.h>

class DianaForth {
public:
    bool begin();                                   // init engine + register words; true on success
    bool ready() const { return _ready; }
    // Evaluate one line. Returns the text the program printed; `errors` = new "? ..." diagnostics.
    String eval(const String& line, int& errors);
    // Run a script from the SD card line by line. Returns lines run; output goes to the HUD log.
    int runFile(const char* path, bool logOutput = true);
    static const char* bootScript() { return "/diana/boot.fs"; }

private:
    bool _ready = false;
    void registerWords();
};

extern DianaForth Forth;
