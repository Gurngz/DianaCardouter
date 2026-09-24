DIANA (D-I-0336-7) for M5Stack Cardputer ADV - SD card package
=================================================================

WHAT IS ON THIS CARD
  Diana.bin              the app (install it with M5Launcher)
  diana/config.json      WiFi + Gemini API key + voice options  <-- edit this
  diana/boot.wav         optional boot music (16 kHz mono WAV); delete to use the chirp
  diana/memory.json      Diana's long-term memory (created automatically; same format as
                         memory/long_term.json in the PC version - you can copy that file here)
  diana/notes.txt        notes Diana takes for you (created automatically)
  diana/chatlog.txt      conversation log (created automatically)

INSTALL (from the Cardputer, no PC cable needed)
  1. Format a microSD card as FAT32 (32 GB or smaller works best) and copy
     everything in this folder to the ROOT of the card.
  2. Open diana/config.json in a text editor: put your WiFi name/password and
     your Gemini API key (free at https://aistudio.google.com/apikey).
     If you skip this, Diana opens a setup portal on first boot instead.
  3. Put the card in the Cardputer ADV. It needs M5Launcher installed
     (https://github.com/bmorcelli/M5Stick-Launcher -> Launcher-m5stack-cardputer.bin,
     flash once over USB with the web flasher or M5Burner).
  4. Turn on the Cardputer, press ENTER on the launcher screen, choose
     "OTA" -> "SD card" (or "SD") -> Diana.bin -> install.
  5. Diana boots. Press any key on the standby screen to wake her.

FIRST-BOOT SETUP PORTAL (if config.json has no key)
  The screen shows a WiFi network "DIANA-SETUP" and its password (diana-xxxx).
  Join it with your phone, open http://192.168.4.1, fill in WiFi + API key,
  tap Save. Diana reboots.

KEYS
  type + ENTER      talk to Diana by text
  TAB               start a voice message (TAB again or silence sends it)
  Fn + ; / Fn + .   scroll the log up / down
  Fn + `  (ESC)     clear the input / stop speaking / go to standby
  /help             list of slash commands (/setup /wifi /key /voice /mute /memory ...)
  /forth <code>     Forth one-liner; /tune lists tunables; /fs boot re-runs diana/boot.fs

SAY "Barnyard Protocol" or "Goodnight Diana" and she powers the device down.
