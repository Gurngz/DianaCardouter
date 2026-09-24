// Application state and entry points defined in main.cpp, shared with the slash-command
// handler (DianaCommands.cpp) and the serial console (DianaConsole.cpp).
#pragma once
#include <Arduino.h>

// state
extern bool   sdOk;            // microSD mounted (voice needs it: audio is streamed through the card)
extern bool   muted;
extern bool   shutdownArmed;   // Barnyard Protocol: power off after the current turn
extern bool   awake;
extern String input;           // the HUD input line
extern bool   menuOpenReq;     // open the settings menu on the next loop
extern bool   musicReq;        // open pixel music on the next loop
extern String musicTrack;
extern String g_lastUserMsg;   // this turn's user message, paired with the reply when persisted

// actions (main.cpp)
void   runTurn(const String& text, const char* audioPath, size_t audioBytes);
void   speakText(const String& text);     // speak with the default style (no mood tag)
void   welcomeProtocol();
void   enterStandby();
void   openSetupPortal();
String applyDeviceSetting(const String& setting, const String& value);
String deviceStatusText();
String addTimer(int seconds, const String& label);
void   refreshStatusBar();
void   appendChatLog(char who, const String& text);

// helpers (main.cpp)
String utf8Truncate(const String& s, size_t maxBytes);
void   utf8Backspace(String& s);
bool   isWakePhrase(const String& lowerText);

// DianaCommands.cpp
bool handleCommand(const String& line);   // true if `line` was a /command (handled or rejected)

// DianaConsole.cpp
void serviceSerialConsole();
bool consoleUiFrozen();                   // '!ui off' diagnostic: skip HUD redraws
