// Setup portal: the Cardputer becomes a WiFi access point and serves a tiny
// page where WiFi credentials, the Gemini API key and voice options can be
// entered from a phone or laptop. Much friendlier than typing a 39-char key.
#pragma once
#include <Arduino.h>

class DianaSetup {
public:
    bool start();                 // start AP + web server
    void stop();
    bool loop();                  // returns true while portal is running; false once saved
    int  clients();
    String url() const { return "http://192.168.4.1"; }
    bool saved() const { return _saved; }

private:
    bool _running = false;
    bool _saved = false;
    String _scanOptions;
    void handleRoot();
    void handleSave();
    void handleScan();
};

extern DianaSetup Setup;
