// Audio: mic recording to SD (16 kHz PCM), PCM/WAV playback from SD through
// the ES8311 codec, and Diana's signature chirps. Mic and speaker share the
// I2S pins on the Cardputer ADV, so they are switched exclusively.
#pragma once
#include <Arduino.h>
#include <functional>
#include "config.h"

class DianaAudio {
public:
    void begin(int volume);
    void setVolume(int v);

    // ── recording (push-to-talk: TAB) ─────────────────────────────────────
    bool startRecording(const char* path, bool autoStop, int vadThreshold, int silenceMs);
    // Call often from the main loop. Returns true while recording continues.
    bool pollRecording();
    // Stops, closes the file. Returns bytes written (0 if nothing usable).
    size_t stopRecording();
    bool   isRecording() const { return _recording; }
    int    level() const { return _level; }          // last chunk RMS (for the meter)
    bool   heardSpeech() const { return _heardSpeech; }

    // ── hands-free listening (continuous VAD; captures whole utterances) ───
    enum ListenResult { LISTEN_IDLE, LISTEN_CAPTURING, LISTEN_DONE };
    bool startListening(const char* path, int vadThreshold, int silenceMs);
    ListenResult pollListening();     // call every loop; writes to path once speech starts
    // Close the utterance file and return bytes, but KEEP the mic running and buffers alive
    // (no I2S restart -> no click) so listening can continue seamlessly for the next utterance.
    size_t takeUtterance();
    void   stopListening();           // fully stop and free
    bool   isListening() const { return _listening; }
    bool   capturing() const { return _capturing; }
    bool   listenHeardSpeech() const { return _lastUtteranceHeard; }  // latched at takeUtterance(): real (loud) speech, not just noise

    // diagnostics
    void bootTest();

    // ── streaming playback: feed PCM as it arrives from the network ───────
    // expectedSeconds: rough length of the reply, used to decide how much to buffer before playing.
    void streamBegin(float expectedSeconds = 0);
    bool streamFeed(const uint8_t* pcm, size_t len, std::function<bool()> tick = nullptr);  // false = aborted
    void streamPoll();               // call often while a stream is open (the playback tick does): keeps the speaker fed
    void streamEnd(std::function<bool()> tick = nullptr);
    int  lastUnderruns() const { return _underruns; }   // direct: queue drains; spool: re-buffer pauses
    int  lastStartDelayMs() const { return _startDelayMs; }  // first byte -> playback start

    // ── playback (blocking; `tick` is called between chunks and may return false to abort) ──
    bool playPcmFile(const char* path, uint32_t rate, std::function<bool()> tick = nullptr);
    bool playWavFile(const char* path, std::function<bool()> tick = nullptr, int volumeOverride = -1);

    // ── signature sounds ────────────────────────────────────────────────
    void chirpBoot();       // "lightning binary chirp" from the PC welcome protocol
    void chirpWake();
    void chirpAck();        // short tick for key/command feedback
    void thinking();        // soft "hmm/mm" filler so the wait before a reply doesn't feel like lag
    void chirpError();
    void chirpTimer();
    void toneMs(float freq, uint32_t ms);

    void speakerMode();     // ensure speaker active (mic off)
    void micMode();         // ensure mic active (speaker off)

private:
    static constexpr int REC_BUFS = 3;
    int16_t* _recBuf[REC_BUFS] = {nullptr, nullptr, nullptr};
    int      _recIdx = 0;
    uint32_t _recStart = 0;
    uint32_t _lastVoiceMs = 0;
    bool     _recording = false;
    bool     _autoStop = true;
    bool     _heardSpeech = false;
    int      _vadThreshold = 900;
    int      _silenceMs = 1500;
    int      _level = 0;
    size_t   _bytesWritten = 0;
    void*    _recFile = nullptr;   // File* (kept opaque to keep SD.h out of the header)
    int      _volume = 200;

    // hands-free listening
    bool     _listening = false;
    bool     _capturing = false;
    int16_t* _preroll[PREROLL_CHUNKS] = {nullptr};
    int      _prerollHead = 0;
    int      _prerollCount = 0;
    int      _listenIdx = 0;
    int      _voiceRun = 0;            // consecutive above-threshold chunks (debounce)
    bool     _listenHeard = false;    // set when a clearly-voiced (loud) chunk seen this utterance
    bool     _lastUtteranceHeard = false; // latched by takeUtterance() so caller can check after reset
    uint32_t _captureStart = 0;
    uint32_t _lastVoiceL = 0;
    int      _listenVad = 550;
    int      _listenSilence = 1200;
    float    _noiseFloor = 400;       // adaptive ambient RMS estimate
    void*    _listenFile = nullptr;
    uint8_t  _recPathBuf[48] = {0};

    int16_t* _playBuf[3] = {nullptr, nullptr, nullptr};
    int16_t* _sBuf[3] = {nullptr, nullptr, nullptr};   // dedicated large streaming buffers
    int      _sIdx = 0;               // rotating buffer index for streamFeed
    int      _sChunk = STREAM_CHUNK_SAMPLES;   // samples per stream buffer (from Tune at streamBegin)
    size_t   _sAccBytes = 0;          // bytes accumulated in the current stream buffer
    int      _sFilled = 0;            // full buffers held during prebuffer
    bool     _sStarted = false;       // playback has begun (prebuffer satisfied)
    bool     _streaming = false;
    int      _underruns = 0;
    // SD spool (jitter buffer): network bytes append to TTS_SPOOL_PATH, playback reads behind them.
    bool     _spool = false;
    void*    _spoolFile = nullptr;    // File*
    uint8_t* _wBuf = nullptr;         // RAM accumulator so the SD sees 4 KB writes, not 300-byte ones
    size_t   _wLen = 0;
    size_t   _flushed = 0;            // bytes written to the file
    size_t   _inBytes = 0;            // bytes received (flushed + in _wBuf)
    size_t   _readPos = 0;            // bytes handed to the speaker
    uint32_t _firstInMs = 0;
    int      _startDelayMs = 0;
    float    _expectSec = 0;
    float    _startBufSec = 0, _startRate = 0;
    float    _pauseAt[4] = {0};         // playback position (s) of the first pauses, for the log
    uint32_t _lastInMs = 0;               // last network bytes (idle input = play the partial tail)
    bool     _pendingPause = false;       // ran dry; counted as a pause only if more audio follows
    void submitStreamBuf(int idx, size_t samples, std::function<bool()> tick);
    void spoolWrite(const uint8_t* p, size_t n);
    void spoolFlush();
    bool spoolReady(size_t have);
    void spoolPump(bool final);
    bool allocPlayBufs();
    void freePlayBufs();
    bool allocRecBufs();
    void freeRecBufs();
    bool streamPcm(void* file, uint32_t rate, bool stereo, size_t bytesTotal, std::function<bool()> tick);
};

extern DianaAudio Audio;
