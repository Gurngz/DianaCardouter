#include "DianaAudio.h"
#include "DianaTune.h"
#include "config.h"
#include "DianaConfig.h"
#include <M5Unified.h>
#include <SD.h>
#include <math.h>

DianaAudio Audio;

// ── ES8311 power sequencing (Cardputer ADV) ──────────────────────────────────
// The pop before every reply was the codec's analog stage being cycled: M5Unified's
// mic-disable callback powers the whole ES8311 down (0x0D=0xFC, CSM off), and its
// speaker-enable callback powers it back up with the DAC already at 0 dB. The output
// bias collapses and recharges through the live amp each time. These replacements keep
// the analog stage (0x0D) and state machine (0x00) up across mode switches, only gate
// the ADC for the mic, and bring the DAC up muted so speakerMode()'s ramp starts from
// silence. Register values are M5Unified's own; only the ordering and the power-down
// differ. Tune.codecHold = 0 restores the library behaviour for an A/B by ear.
static constexpr uint8_t ES8311_ADDR = 0x18;

// (register, value) pairs. Counted, not zero-terminated: register 0x00 (RESET/CSM) is a real
// register and the first entry of most sequences, so a 0 terminator stopped every write before it began.
template <size_t N>
static bool es8311Write(const uint8_t (&seq)[N]) {
    static_assert(N % 2 == 0, "ES8311 sequence must be (register, value) pairs");
    bool ok = true;
    for (size_t i = 0; i < N; i += 2) {
        bool w = false;
        for (int r = 0; r < 3 && !w; ++r) w = M5.In_I2C.writeRegister8(ES8311_ADDR, seq[i], seq[i + 1], 100000);
        ok &= w;
    }
    return ok;
}

static bool micEnableCb(void*, bool enabled) {
    static uint8_t on[] = {
        0x00, 0x80,   // CSM power on
        0x01, 0xBA,   // clock manager (ADC)
        0x02, 0x18,   // MULT_PRE=3
        0x0D, 0x01,   // analog up (already up when codecHold)
        0x0E, 0x02,   // PGA + ADC modulator on
        0x14, 0x10,   // Mic1p-Mic1n, PGA min
        0x17, 0xBF,   // ADC volume 0 dB
        0x1C, 0x6A,   // ADC EQ bypass, DC-offset cancel
    };
    static const uint8_t offHold[] = {
        0x0E, 0x6A,   // PGA + ADC off; analog stage and CSM stay powered
    };
    static const uint8_t offFull[] = {
        0x0D, 0xFC, 0x0E, 0x6A, 0x00, 0x00,   // M5Unified default: full power-down
    };
    if (enabled) {
        on[3] = (Tune.popFix & 1) ? 0xBF : 0xBA;   // pop_fix 1: keep the DAC clocked while listening
        bool ok = es8311Write(on);
        if ((Tune.popFix & 2) && !(Tune.popFix & 8)) M5.In_I2C.writeRegister8(ES8311_ADDR, 0x32, 0x00, 100000);   // DAC muted while listening (8 overrides: volume never moves)
        return ok;
    }
    return Tune.codecHold ? es8311Write(offHold) : es8311Write(offFull);
}

static bool speakerEnableCb(void*, bool enabled) {
    if (!enabled) return true;   // same as M5Unified: nothing on disable
    static uint8_t onMuted[] = {
        0x00, 0x80,   // CSM power on
        0x01, 0xB5,   // clock manager (DAC)
        0x02, 0x18,   // MULT_PRE=3
        0x0D, 0x01,   // analog up
        0x32, 0x00,   // DAC volume MUTED before the DAC and driver come up
        0x12, 0x00,   // DAC power up
        0x13, 0x10,   // HP driver on
        0x37, 0x08,   // bypass DAC EQ
    };
    static const uint8_t onDefault[] = {
        0x00, 0x80, 0x01, 0xB5, 0x02, 0x18, 0x0D, 0x01,
        0x12, 0x00, 0x13, 0x10, 0x32, 0xBF, 0x37, 0x08,   // M5Unified default order: 0 dB at once
    };
    onMuted[3]  = (Tune.popFix & 1) ? 0xBF : 0xB5;   // pop_fix 1: ADC clocks stay on too, so nothing toggles
    onMuted[9]  = (Tune.popFix & 8) ? 0xBF : 0x00;   // pop_fix 8: DAC volume stays at 0 dB, never stepped
    onMuted[13] = (Tune.popFix & 4) ? 0x00 : 0x10;   // pop_fix 4: driver stays off; speakerMode enables it later
    return Tune.codecHold ? es8311Write(onMuted) : es8311Write(onDefault);
}

// setCallback() is protected in M5Unified (M5Unified::begin wires the board callbacks).
// Naming it through a derived class yields a pointer-to-member we may apply to M5.Mic /
// M5.Speaker - standard C++, no library edit.
struct MicCbAccess : m5::Mic_Class {
    static void set(m5::Mic_Class& m, bool (*f)(void*, bool)) { (m.*(&MicCbAccess::setCallback))(nullptr, f); }
};
struct SpeakerCbAccess : m5::Speaker_Class {
    static void set(m5::Speaker_Class& s, bool (*f)(void*, bool)) { (s.*(&SpeakerCbAccess::setCallback))(nullptr, f); }
};

void DianaAudio::begin(int volume) {
    _volume = volume;
    if (M5.getBoard() == m5::board_t::board_M5CardputerADV) {
        MicCbAccess::set(M5.Mic, micEnableCb);
        SpeakerCbAccess::set(M5.Speaker, speakerEnableCb);
        Serial.println("[AUDIO] ES8311 callbacks: Diana power sequencing (codec_hold)");
    }
    // Boost the built-in MEMS mic so Diana hears farther (library default magnification is 16).
    {
        auto mc = M5.Mic.config();
        mc.magnification = Config.micGain;
        mc.noise_filter_level = 0;
        M5.Mic.config(mc);
    }
    bool ok = M5.Speaker.begin();
    M5.Speaker.setVolume(volume);
    Serial.printf("[AUDIO] speaker begin=%d enabled=%d running=%d vol=%d mic_gain=%d\n",
                  ok, M5.Speaker.isEnabled(), M5.Speaker.isRunning(), volume, Config.micGain);
}

void DianaAudio::bootTest() {
    // A loud, unmistakable startup melody so we can confirm the speaker physically works.
    speakerMode();
    uint8_t v = _volume;
    M5.Speaker.setVolume(255);
    const float notes[] = {523.25f, 659.25f, 783.99f, 1046.5f, 783.99f, 1046.5f};
    for (float f : notes) { M5.Speaker.tone(f, 140, 0, true); delay(160); }
    uint32_t t0 = millis();
    while (M5.Speaker.isPlaying() && millis() - t0 < 1000) delay(10);
    M5.Speaker.setVolume(v);
}

void DianaAudio::setVolume(int v) {
    _volume = constrain(v, 0, 170);   // cap peak current so the speaker can't brown out the rail
    M5.Speaker.setVolume(_volume);
}

void DianaAudio::speakerMode() {
    // The mic and speaker share the ES8311's I2S pins. At boot (and after mic use) the speaker
    // can report isRunning()==true yet produce NO sound - the shared I2S is left in a dead state.
    // A clean end()+begin() reliably restores real output (verified on device: skipping this gave
    // total silence with perfect codec regs; a full re-init played fine). So always hard re-init.
    if ((Tune.popFix & 2) && !(Tune.popFix & 8)) M5.In_I2C.writeRegister8(0x18, 0x32, 0x00, 100000);   // mute BEFORE the clock stops
    if (Tune.popFix & 4) M5.In_I2C.writeRegister8(0x18, 0x13, 0x00, 100000);   // driver off across the switch
    if (M5.Mic.isRunning()) M5.Mic.end();
    M5.Speaker.end();
    delay(20);
    M5.Speaker.begin();                                   // re-runs the ES8311 enable callback
    M5.Speaker.setVolume(_volume);
    if (Tune.popFix & 4) {                                // clocks are running again: settle, then driver on
        delay(40);
        M5.In_I2C.writeRegister8(0x18, 0x13, 0x10, 100000);
        delay(20);
    }
    if (Tune.popFix & 8) return;                          // volume never moves: nothing to ramp
    // Soft-start: the enable callback leaves the DAC muted (codec_hold) - ramp it up so sound
    // eases in. With codec_hold=0 the library callback snaps to 0 dB first; the re-mute covers that.
    M5.In_I2C.writeRegister8(0x18, 0x32, 0x00, 100000);   // DAC volume -> mute
    delay(12);
    for (int v = 0x40; v < 0xBF; v += 0x1A) {             // ramp up, ~2 ms/step
        M5.In_I2C.writeRegister8(0x18, 0x32, (uint8_t)v, 100000);
        delay(2);
    }
    M5.In_I2C.writeRegister8(0x18, 0x32, 0xBF, 100000);   // 0 dB target
}

void DianaAudio::micMode() {
    // Original minimal switch (this is the version that listened cleanly). The later
    // "hiss fix" that poked ES8311 output regs (0x32/0x13/0x12) here every time she
    // started listening is what introduced the rhythmic "helicopter" chop, so it's gone.
    if (M5.Speaker.isRunning()) {
        M5.Speaker.stop();
        M5.Speaker.end();
    }
    if (!M5.Mic.isRunning()) M5.Mic.begin();
}

// ───────────────────────────── recording ─────────────────────────────────
bool DianaAudio::allocRecBufs() {
    for (int i = 0; i < REC_BUFS; ++i) {
        if (!_recBuf[i]) _recBuf[i] = (int16_t*)malloc(REC_CHUNK_SAMPLES * sizeof(int16_t));
        if (!_recBuf[i]) { freeRecBufs(); return false; }
        memset(_recBuf[i], 0, REC_CHUNK_SAMPLES * sizeof(int16_t));
    }
    return true;
}

void DianaAudio::freeRecBufs() {
    for (int i = 0; i < REC_BUFS; ++i) { free(_recBuf[i]); _recBuf[i] = nullptr; }
}

bool DianaAudio::startRecording(const char* path, bool autoStop, int vadThreshold, int silenceMs) {
    if (_recording) return true;
    if (!allocRecBufs()) return false;
    File* f = new File(SD.open(path, FILE_WRITE));
    if (!f || !*f) { delete f; freeRecBufs(); return false; }
    _recFile = f;
    _autoStop = autoStop;
    _vadThreshold = vadThreshold;
    _silenceMs = silenceMs;
    _heardSpeech = false;
    _bytesWritten = 0;
    _level = 0;
    _recIdx = 0;
    micMode();
    _recStart = millis();
    _lastVoiceMs = _recStart;
    // prime the queue with two buffers; buffer (i-2) is complete once record(i) returns
    M5.Mic.record(_recBuf[0], REC_CHUNK_SAMPLES, REC_RATE);
    M5.Mic.record(_recBuf[1], REC_CHUNK_SAMPLES, REC_RATE);
    _recIdx = 2;
    _recording = true;
    return true;
}

bool DianaAudio::pollRecording() {
    if (!_recording) return false;
    File* f = (File*)_recFile;
    // Queue the next buffer; this blocks (~32 ms) until a slot frees, after which
    // the buffer two steps back is guaranteed complete.
    int cur = _recIdx % REC_BUFS;
    if (!M5.Mic.record(_recBuf[cur], REC_CHUNK_SAMPLES, REC_RATE)) {
        delay(5);
        return true;
    }
    int done = (_recIdx + 1) % REC_BUFS;   // == (_recIdx - 2) mod 3
    _recIdx++;
    int16_t* d = _recBuf[done];
    // level (RMS)
    uint64_t acc = 0;
    for (int i = 0; i < REC_CHUNK_SAMPLES; ++i) acc += (int32_t)d[i] * d[i];
    _level = (int)sqrt((double)(acc / REC_CHUNK_SAMPLES));
    uint32_t now = millis();
    if (_level > _vadThreshold) {
        _heardSpeech = true;
        _lastVoiceMs = now;
    }
    f->write((uint8_t*)d, REC_CHUNK_SAMPLES * sizeof(int16_t));
    _bytesWritten += REC_CHUNK_SAMPLES * sizeof(int16_t);

    if (now - _recStart > (uint32_t)Tune.recMaxSeconds * 1000UL) return false;
    if (_autoStop && _heardSpeech && (now - _lastVoiceMs) > (uint32_t)_silenceMs) return false;
    if (_autoStop && !_heardSpeech && (now - _recStart) > 6000) return false;   // nothing said
    return true;
}

size_t DianaAudio::stopRecording() {
    if (!_recording) return 0;
    _recording = false;
    // let the queued buffers finish (they're discarded)
    uint32_t t0 = millis();
    while (M5.Mic.isRecording() && millis() - t0 < 300) delay(1);
    File* f = (File*)_recFile;
    if (f) {
        f->flush();
        f->close();
        delete f;
    }
    _recFile = nullptr;
    if (M5.Mic.isRunning()) M5.Mic.end();   // must fully stop the capture task before freeing its buffers
    freeRecBufs();
    speakerMode();
    size_t bytes = _bytesWritten;
    _bytesWritten = 0;
    return bytes;
}

// ───────────────────────── hands-free listening ──────────────────────────
// Continuously reads the mic; keeps a short pre-roll ring so the start of an
// utterance (the wake word) isn't clipped. When the chunk energy crosses the
// threshold it opens the file, flushes the pre-roll and records until silence.
bool DianaAudio::startListening(const char* path, int vadThreshold, int silenceMs) {
    if (_listening) return true;
    if (!allocRecBufs()) return false;
    for (int i = 0; i < PREROLL_CHUNKS; ++i) {
        if (!_preroll[i]) _preroll[i] = (int16_t*)malloc(REC_CHUNK_SAMPLES * sizeof(int16_t));
        if (!_preroll[i]) { stopListening(); return false; }
    }
    _listenVad = vadThreshold;
    _listenSilence = silenceMs;
    _noiseFloor = vadThreshold * 0.6f;   // seed; adapts to the room below
    _prerollHead = _prerollCount = 0;
    _voiceRun = 0;
    _listenHeard = false;
    _capturing = false;
    _level = 0;
    _bytesWritten = 0;
    _listenFile = nullptr;
    _listenIdx = 0;
    strncpy((char*)_recPathBuf, path, sizeof(_recPathBuf) - 1);
    _recPathBuf[sizeof(_recPathBuf) - 1] = 0;
    micMode();
    M5.Mic.record(_recBuf[0], REC_CHUNK_SAMPLES, REC_RATE);
    M5.Mic.record(_recBuf[1], REC_CHUNK_SAMPLES, REC_RATE);
    _listenIdx = 2;
    _listening = true;
    return true;
}

DianaAudio::ListenResult DianaAudio::pollListening() {
    if (!_listening) return LISTEN_IDLE;
    int cur = _listenIdx % REC_BUFS;
    if (!M5.Mic.record(_recBuf[cur], REC_CHUNK_SAMPLES, REC_RATE)) { delay(3); return _capturing ? LISTEN_CAPTURING : LISTEN_IDLE; }
    int done = (_listenIdx + 1) % REC_BUFS;   // completed buffer (two steps back)
    _listenIdx++;
    int16_t* d = _recBuf[done];

    uint64_t acc = 0;
    for (int i = 0; i < REC_CHUNK_SAMPLES; ++i) acc += (int32_t)d[i] * d[i];
    _level = (int)sqrt((double)(acc / REC_CHUNK_SAMPLES));
    uint32_t now = millis();
    // Adaptive thresholds relative to the room's noise floor, so a high mic gain or a
    // noisy room doesn't read as constant speech (which made her "stuck listening").
    int trigger = (int)(_noiseFloor * 2.2f) + 220;
    if (trigger < _listenVad) trigger = _listenVad;
    int silenceLvl = (int)(_noiseFloor * 1.4f) + 120;
    bool voiced = _level > (_capturing ? silenceLvl : trigger);

    if (!_capturing) {
        // track the ambient floor from quiet chunks only (EMA), ignoring loud/voiced ones
        if (_level < trigger) _noiseFloor += (_level - _noiseFloor) * 0.10f;
        // keep this chunk in the pre-roll ring
        memcpy(_preroll[_prerollHead], d, REC_CHUNK_SAMPLES * sizeof(int16_t));
        _prerollHead = (_prerollHead + 1) % PREROLL_CHUNKS;
        if (_prerollCount < PREROLL_CHUNKS) _prerollCount++;
        _voiceRun = voiced ? _voiceRun + 1 : 0;
        if (_voiceRun >= 2) {                       // debounce: two voiced chunks in a row
            File* f = new File(SD.open((const char*)_recPathBuf, FILE_WRITE));
            if (!f || !*f) { delete f; return LISTEN_IDLE; }
            _listenFile = f;
            // flush pre-roll oldest -> newest
            int start = (_prerollHead - _prerollCount + PREROLL_CHUNKS) % PREROLL_CHUNKS;
            for (int i = 0; i < _prerollCount; ++i) {
                int idx = (start + i) % PREROLL_CHUNKS;
                f->write((uint8_t*)_preroll[idx], REC_CHUNK_SAMPLES * sizeof(int16_t));
                _bytesWritten += REC_CHUNK_SAMPLES * sizeof(int16_t);
            }
            _capturing = true;
            _captureStart = now;
            _lastVoiceL = now;
        }
        return LISTEN_IDLE;
    }

    // capturing
    File* f = (File*)_listenFile;
    f->write((uint8_t*)d, REC_CHUNK_SAMPLES * sizeof(int16_t));
    _bytesWritten += REC_CHUNK_SAMPLES * sizeof(int16_t);
    if (_level > trigger) _listenHeard = true;   // saw a clearly-loud chunk -> real speech, not noise
    if (voiced) _lastVoiceL = now;
    if (now - _lastVoiceL > (uint32_t)_listenSilence) return LISTEN_DONE;
    if (now - _captureStart > (uint32_t)Tune.recMaxSeconds * 1000UL) return LISTEN_DONE;
    return LISTEN_CAPTURING;
}

size_t DianaAudio::takeUtterance() {
    if (!_listening) return 0;
    File* f = (File*)_listenFile;
    if (f) { f->flush(); f->close(); delete f; _listenFile = nullptr; }
    // Reset capture state but keep the mic running and buffers allocated -> no I2S restart
    // (no click) so we seamlessly listen for the next utterance.
    _capturing = false;
    _voiceRun = 0;
    _prerollHead = _prerollCount = 0;
    bool heard = _listenHeard;
    _listenHeard = false;
    size_t b = _bytesWritten;
    _bytesWritten = 0;
    _lastUtteranceHeard = heard;
    return b;
}

void DianaAudio::stopListening() {
    if (_listenFile) { File* f = (File*)_listenFile; f->close(); delete f; _listenFile = nullptr; }
    if (M5.Mic.isRunning()) M5.Mic.end();   // stop capture task before freeing buffers (heap safety)
    _listening = false;
    _capturing = false;
    freeRecBufs();
    for (int i = 0; i < PREROLL_CHUNKS; ++i) { free(_preroll[i]); _preroll[i] = nullptr; }
    _bytesWritten = 0;
}

// ───────────────────────────── playback ──────────────────────────────────
bool DianaAudio::allocPlayBufs() {
    for (int i = 0; i < 3; ++i) {
        if (!_playBuf[i]) _playBuf[i] = (int16_t*)malloc(PLAY_CHUNK_SAMPLES * 2 * sizeof(int16_t)); // room for stereo
        if (!_playBuf[i]) { freePlayBufs(); return false; }
    }
    return true;
}

void DianaAudio::freePlayBufs() {
    for (int i = 0; i < 3; ++i) { free(_playBuf[i]); _playBuf[i] = nullptr; }
}

bool DianaAudio::streamPcm(void* filePtr, uint32_t rate, bool stereo, size_t bytesTotal, std::function<bool()> tick) {
    File* f = (File*)filePtr;
    if (!allocPlayBufs()) return false;
    speakerMode();
    const int ch = 0;
    int idx = 0;
    size_t left = bytesTotal;
    bool aborted = false;
    size_t chunkBytes = PLAY_CHUNK_SAMPLES * sizeof(int16_t) * (stereo ? 2 : 1);
    while (left > 0) {
        size_t want = left < chunkBytes ? left : chunkBytes;
        int16_t* buf = _playBuf[idx];
        size_t r = f->read((uint8_t*)buf, want);
        if (r == 0) break;
        left -= r;
        // wait until a slot is free (max 2 queued per channel)
        while (M5.Speaker.isPlaying(ch) >= 2) {
            delay(2);
            if (tick && !tick()) { aborted = true; break; }
        }
        if (aborted) break;
        M5.Speaker.playRaw(buf, r / sizeof(int16_t), rate, stereo, 1, ch, false);
        idx = (idx + 1) % 3;
        if (tick && !tick()) { aborted = true; break; }
    }
    if (aborted) {
        M5.Speaker.stop(ch);
    } else {
        uint32_t t0 = millis();
        while (M5.Speaker.isPlaying(ch) && millis() - t0 < 10000) {
            delay(5);
            if (tick && !tick()) { M5.Speaker.stop(ch); break; }
        }
    }
    delay(20);
    freePlayBufs();
    return !aborted;
}

// ───────────────────── streaming playback (network -> speaker) ───────────
// The M5 speaker holds only 2 queued slots per channel, so smoothness comes from
// large buffers: STREAM_CHUNK_SAMPLES each, with a short prebuffer before playback
// starts so brief network stalls don't underrun the DAC.
void DianaAudio::streamBegin(float expectedSeconds) {
    if (_spoolFile) { File* f = (File*)_spoolFile; f->close(); delete f; _spoolFile = nullptr; }   // retry of a failed attempt
    speakerMode();
    _sChunk = Tune.streamChunkSamples;       // snapshot: the whole stream uses one size
    for (int i = 0; i < 3; ++i) {
        if (!_sBuf[i]) _sBuf[i] = (int16_t*)malloc(_sChunk * sizeof(int16_t));
    }
    if (!_sBuf[0] || !_sBuf[1] || !_sBuf[2]) {      // no PSRAM: 14.4 KB can fail mid-TLS
        Serial.printf("[AUDIO] stream buffers unavailable (heap=%u)\n", ESP.getFreeHeap());
        for (int i = 0; i < 3; ++i) { free(_sBuf[i]); _sBuf[i] = nullptr; }
        _streaming = false;
        return;
    }
    _sIdx = 0;
    _sAccBytes = 0;
    _sFilled = 0;
    _sStarted = false;
    _underruns = 0;
    _startDelayMs = 0;
    _streaming = true;

    // SD jitter buffer. The network can deliver slower than real time (measured 2026-09-25:
    // 0.62-0.86x on Live native audio), and three RAM buffers hold at most ~1 s, so a slow
    // stream clicked every buffer. Spooling to the card lets playback wait until it can finish.
    _spool = false;
    _expectSec = expectedSeconds;
    _wLen = _flushed = _inBytes = _readPos = 0;
    _firstInMs = 0;
    _lastInMs = 0;
    _pendingPause = false;
    if (Tune.spool) {
        if (!_wBuf) _wBuf = (uint8_t*)malloc(4096);
        File* f = _wBuf ? new File(SD.open(TTS_SPOOL_PATH, "w+")) : nullptr;
        if (f && *f) { _spoolFile = f; _spool = true; }
        else { if (f) delete f; free(_wBuf); _wBuf = nullptr;
               Serial.println("[AUDIO] spool unavailable - direct streaming"); }
    }
}

// ───────────────────────── SD spool (jitter buffer) ─────────────────────
void DianaAudio::spoolFlush() {
    if (!_wLen || !_spoolFile) return;
    File* f = (File*)_spoolFile;
    f->seek(_flushed);
    size_t w = f->write(_wBuf, _wLen);
    _flushed += w;
    _wLen = 0;
}

void DianaAudio::spoolWrite(const uint8_t* p, size_t n) {
    _lastInMs = millis();
    if (_pendingPause) {                   // more audio after running dry: that was a real pause
        if (_underruns < 4) _pauseAt[_underruns] = _readPos / (TTS_RATE * 2.0f);
        _underruns++;
        _pendingPause = false;
    }
    while (n) {
        size_t k = 4096 - _wLen; if (k > n) k = n;
        memcpy(_wBuf + _wLen, p, k);
        _wLen += k; p += k; n -= k; _inBytes += k;
        if (_wLen == 4096) spoolFlush();
    }
}

// Start (or resume) once the buffered audio can carry playback to the end of the reply.
// With arrival rate r (seconds of audio per second) and R seconds still to arrive, playback
// never overtakes arrival when buffered >= R * (1/r - 1). A fast link (r >= 1) needs only the margin.
bool DianaAudio::spoolReady(size_t have) {
    const float bps = TTS_RATE * 2.0f;
    float bufSec = have / bps;
    float minSec = (float)Tune.streamPrebuffer * _sChunk / TTS_RATE;
    if (bufSec < minSec) return false;
    float el = (millis() - _firstInMs) / 1000.0f;
    if (el < 0.4f) return false;                         // too early to know the rate
    float inSec = _inBytes / bps;
    float rate = inSec / el * 0.9f;                      // arrival slows mid-reply more often than it speeds up
    // The length estimate is from the text; if more audio than that has already arrived,
    // assume the reply runs on rather than treating it as nearly finished.
    if (inSec > _expectSec * 0.9f) _expectSec = inSec * 1.3f;
    float remain = _expectSec - inSec; if (remain < 0) remain = 0;
    float need = Tune.jitterMarginMs / 1000.0f;
    if (rate < 1.0f && rate > 0.05f) need += remain * (1.0f / rate - 1.0f);
    if (_underruns > 0 && need < 1.0f) need = 1.0f;     // after a pause, resume with a real cushion
    // Never make her silent for longer than jitter_max_ms before the FIRST word: on a very
    // slow link a long reply would otherwise wait 10+ s. Past the cap, start and accept a pause.
    bool capped = !_startDelayMs && Tune.jitterMaxMs > 0 && el * 1000.0f >= Tune.jitterMaxMs;
    if (bufSec < need && !capped) return false;
    if (!_startDelayMs) { _startBufSec = bufSec; _startRate = rate / 0.9f; }   // log the FIRST start
    return true;
}

// Non-blocking: top the speaker queue up from the file. `final` = no more bytes are coming.
void DianaAudio::spoolPump(bool final) {
    if (!_spool || !_sBuf[0]) return;
    File* f = (File*)_spoolFile;
    const size_t chunkBytes = (size_t)_sChunk * sizeof(int16_t);
    for (;;) {
        size_t have = _inBytes - _readPos;
        if (!_sStarted) {
            if (!(final ? have > 0 : spoolReady(have))) return;
            _sStarted = true;
            if (!_startDelayMs) _startDelayMs = (int)(millis() - _firstInMs);
        }
        int queued = M5.Speaker.isPlaying(0);
        if (queued >= 2) return;
        if (have < chunkBytes && !final) {
            // Input idle for 150 ms with a partial chunk left: it is almost certainly the end of
            // the reply, so play it now instead of leaving a gap before the last syllable.
            bool inputIdle = have > 0 && millis() - _lastInMs > 150;
            if (!inputIdle) {
                if (queued == 0 && !_pendingPause) { _pendingPause = true; _sStarted = false; }   // ran dry: re-buffer
                return;
            }
        }
        size_t n = have < chunkBytes ? have : chunkBytes;
        n &= ~(size_t)1;
        if (!n) return;
        if (_readPos + n > _flushed) spoolFlush();
        f->seek(_readPos);
        size_t got = f->read((uint8_t*)_sBuf[_sIdx], n);
        if (got < 2) return;
        got &= ~(size_t)1;
        M5.Speaker.playRaw(_sBuf[_sIdx], got / 2, TTS_RATE, false, 1, 0, false);
        _sIdx = (_sIdx + 1) % 3;
        _readPos += got;
    }
}

void DianaAudio::streamPoll() {
    if (_streaming && _spool) spoolPump(false);
}

void DianaAudio::submitStreamBuf(int idx, size_t samples, std::function<bool()> tick) {
    if (_sStarted && M5.Speaker.isPlaying(0) == 0) _underruns++;   // queue drained = audible gap
    while (M5.Speaker.isPlaying(0) >= 2) {
        delay(2);
        if (tick) tick();          // keep the UI/keys alive; abort is handled by the caller
    }
    M5.Speaker.playRaw(_sBuf[idx], samples, TTS_RATE, false, 1, 0, false);
}

bool DianaAudio::streamFeed(const uint8_t* pcm, size_t len, std::function<bool()> tick) {
    if (!_streaming || !_sBuf[0] || !_sBuf[1] || !_sBuf[2]) return false;
    if (_spool) {
        if (!_firstInMs) _firstInMs = millis();
        spoolWrite(pcm, len);
        spoolPump(false);
        return true;
    }
    const size_t chunkBytes = (size_t)_sChunk * sizeof(int16_t);
    while (len > 0) {
        int16_t* buf = _sBuf[_sIdx];
        size_t space = chunkBytes - _sAccBytes;
        size_t n = len < space ? len : space;
        memcpy((uint8_t*)buf + _sAccBytes, pcm, n);
        _sAccBytes += n;
        pcm += n;
        len -= n;
        if (_sAccBytes == chunkBytes) {
            if (!_sStarted) {
                // Prebuffer: hold the first STREAM_PREBUFFER buffers, then flush them
                // so playback begins with a full cushion already queued.
                _sFilled++;
                _sIdx = (_sIdx + 1) % 3;
                _sAccBytes = 0;
                if (_sFilled >= Tune.streamPrebuffer) {
                    for (int k = 0; k < _sFilled; ++k) {
                        int bi = (_sIdx - _sFilled + k + 3) % 3;
                        submitStreamBuf(bi, _sChunk, tick);
                    }
                    _sStarted = true;
                }
            } else {
                submitStreamBuf(_sIdx, _sChunk, tick);
                _sIdx = (_sIdx + 1) % 3;
                _sAccBytes = 0;
            }
            if (tick && !tick()) { M5.Speaker.stop(0); _streaming = false; return false; }
        }
    }
    return true;
}

void DianaAudio::streamEnd(std::function<bool()> tick) {
    if (!_streaming) { return; }
    if (_spool) {
        // Everything has arrived: play the rest of the file, then close it.
        spoolFlush();
        size_t left = _inBytes - _readPos;
        uint32_t limit = (uint32_t)(left / (TTS_RATE * 2.0f) * 1000) + 5000;
        uint32_t t0 = millis();
        bool aborted = false;
        while ((_readPos < _inBytes || M5.Speaker.isPlaying(0)) && millis() - t0 < limit) {
            spoolPump(true);
            delay(5);
            if (tick && !tick()) { aborted = true; break; }
        }
        if (aborted || M5.Speaker.isPlaying(0)) M5.Speaker.stop(0);
        Serial.printf("[AUDIO] spool %.1fs audio, start after %dms (buffered %.1fs, arrival %.2fx, expected %.1fs), pauses=%d at",
                      _inBytes / (TTS_RATE * 2.0f), _startDelayMs, _startBufSec, _startRate, _expectSec, _underruns);
        for (int i = 0; i < _underruns && i < 4; ++i) Serial.printf(" %.1fs", _pauseAt[i]);
        Serial.println();
        File* f = (File*)_spoolFile; f->close(); delete f; _spoolFile = nullptr;
        free(_wBuf); _wBuf = nullptr;
        _spool = false;
        delay(20);
        for (int i = 0; i < 3; ++i) { free(_sBuf[i]); _sBuf[i] = nullptr; }
        _streaming = false;
        return;
    }
    // flush any buffers still held from prebuffer, plus the partial tail
    if (!_sStarted) {
        for (int k = 0; k < _sFilled; ++k) {
            int bi = (_sIdx - _sFilled + k + 3) % 3;
            submitStreamBuf(bi, _sChunk, tick);
        }
        _sStarted = true;
    }
    if (_sAccBytes > 0) {
        submitStreamBuf(_sIdx, _sAccBytes / sizeof(int16_t), tick);
        _sAccBytes = 0;
    }
    uint32_t t0 = millis();
    while (M5.Speaker.isPlaying(0) && millis() - t0 < 12000) {
        delay(5);
        if (tick && !tick()) { M5.Speaker.stop(0); break; }
    }
    if (M5.Speaker.isPlaying(0)) M5.Speaker.stop(0);   // timed out: don't free buffers the DAC still reads
    delay(20);
    for (int i = 0; i < 3; ++i) { free(_sBuf[i]); _sBuf[i] = nullptr; }
    _streaming = false;
}

bool DianaAudio::playPcmFile(const char* path, uint32_t rate, std::function<bool()> tick) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    size_t total = f.size();
    bool ok = total > 0 && streamPcm(&f, rate, false, total, tick);
    f.close();
    return ok;
}

bool DianaAudio::playWavFile(const char* path, std::function<bool()> tick, int volumeOverride) {
    File f = SD.open(path, FILE_READ);
    if (!f) return false;
    uint8_t hdr[12];
    if (f.read(hdr, 12) != 12 || memcmp(hdr, "RIFF", 4) || memcmp(hdr + 8, "WAVE", 4)) { f.close(); return false; }
    uint16_t channels = 1, bits = 16;
    uint32_t rate = 16000;
    uint32_t dataLen = 0;
    bool haveFmt = false;
    // walk chunks
    while (f.available()) {
        uint8_t ch[8];
        if (f.read(ch, 8) != 8) break;
        uint32_t len = ch[4] | (ch[5] << 8) | (ch[6] << 16) | ((uint32_t)ch[7] << 24);
        if (!memcmp(ch, "fmt ", 4)) {
            uint8_t fmt[16];
            if (f.read(fmt, 16) != 16) break;
            uint16_t audioFormat = fmt[0] | (fmt[1] << 8);
            channels = fmt[2] | (fmt[3] << 8);
            rate = fmt[4] | (fmt[5] << 8) | (fmt[6] << 16) | ((uint32_t)fmt[7] << 24);
            bits = fmt[14] | (fmt[15] << 8);
            haveFmt = (audioFormat == 1 && bits == 16 && (channels == 1 || channels == 2));
            if (len > 16) f.seek(f.position() + (len - 16));
        } else if (!memcmp(ch, "data", 4)) {
            dataLen = len;
            break;
        } else {
            f.seek(f.position() + len + (len & 1));
        }
    }
    if (!haveFmt || dataLen == 0) { f.close(); return false; }
    if (dataLen > f.size() - f.position()) dataLen = f.size() - f.position();
    // streamPcm() -> speakerMode() applies _volume, so the override must go through _volume.
    int oldVol = _volume;
    if (volumeOverride >= 0) _volume = volumeOverride;
    bool ok = streamPcm(&f, rate, channels == 2, dataLen, tick);
    _volume = oldVol;
    M5.Speaker.setVolume(_volume);
    f.close();
    return ok;
}


// ───────────────────────────── chirps ────────────────────────────────────
void DianaAudio::toneMs(float freq, uint32_t ms) {
    speakerMode();
    M5.Speaker.tone(freq, ms, 1, true);
    delay(ms);
}

void DianaAudio::chirpBoot() {
    // "lightning binary chirp": rapid alternating high/low blips then a rising sweep
    speakerMode();
    const float seq[] = {1760, 2349, 1760, 2637, 1975, 2793, 2349, 3136};
    for (float f : seq) { M5.Speaker.tone(f, 28, 1, true); delay(34); }
    for (int i = 0; i < 14; ++i) { M5.Speaker.tone(900 + i * 140, 18, 1, true); delay(20); }
    M5.Speaker.tone(2637, 120, 1, true);
    delay(140);
}

void DianaAudio::chirpWake() {
    speakerMode();
    M5.Speaker.tone(1319, 60, 1, true); delay(70);
    M5.Speaker.tone(1760, 60, 1, true); delay(70);
    M5.Speaker.tone(2637, 140, 1, true); delay(150);
}

void DianaAudio::chirpAck() {
    speakerMode();
    M5.Speaker.tone(2093, 25, 1, true);
}

// Soft "mm-hmm" hum played the instant a turn starts, so the wait before her reply feels like
// she's thinking, not lagging. Low, gentle tones (not a beep), kept short so it barely adds delay.
void DianaAudio::thinking() {
    speakerMode();
    int v = _volume;
    M5.Speaker.setVolume(_volume < 110 ? _volume : 110);   // soft
    M5.Speaker.tone(250, 160, 0, true); delay(120);        // "mm"
    M5.Speaker.tone(300, 220, 0, true); delay(170);        // "hmm"
    M5.Speaker.setVolume(v);
}

void DianaAudio::chirpError() {
    speakerMode();
    M5.Speaker.tone(440, 90, 1, true); delay(100);
    M5.Speaker.tone(330, 160, 1, true); delay(170);
}

void DianaAudio::chirpTimer() {
    speakerMode();
    for (int r = 0; r < 3; ++r) {
        M5.Speaker.tone(2093, 80, 1, true); delay(100);
        M5.Speaker.tone(2637, 80, 1, true); delay(100);
        M5.Speaker.tone(3136, 160, 1, true); delay(260);
    }
}
