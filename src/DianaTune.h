// Runtime tunables: the audio/conversation constants from config.h that are worth
// adjusting on the device without a reflash. Defaults come from config.h; values are
// changed from Forth (`s" stream_chunk" 3200 tune!`, see DianaForth.cpp) and take
// effect on the next stream / request.
#pragma once
#include "config.h"

struct DianaTune {
    int streamChunkSamples = STREAM_CHUNK_SAMPLES;   // TTS playback buffer (samples @ 24 kHz); bigger = deeper jitter cushion, later start
    int streamPrebuffer    = STREAM_PREBUFFER;       // buffers held before playback begins
    int replyMaxTokens     = REPLY_MAX_TOKENS;       // Gemini maxOutputTokens
    int recMaxSeconds      = REC_MAX_SECONDS;        // hard cap on one recording / utterance
    int historyMaxTurns    = HISTORY_MAX_TURNS;      // exchanges kept in context
    int historyMaxChars    = HISTORY_MAX_CHARS;
    int spool              = 1;   // 1 = buffer streamed voice on the SD card and start when it can finish unbroken; 0 = direct
    int jitterMarginMs     = 500; // extra head start on top of the computed need
    int jitterMaxMs        = 4000; // longest wait before the first word, even if a pause may follow (0 = no cap)
    int speechCps          = 15;  // spoken characters per second, for estimating reply length (measured 14.5-17 on Live)
    int bootVolumePct      = 49;  // boot music, % of speaker volume (was 75; cut 35% on request 2026-09-25)
    int popFix             = 0;   // A/B bits for the pop before speech: 1 = keep DAC+ADC clocks on in both modes,
                                  // 2 = mute the DAC before the mic->speaker switch (and while listening),
                                  // 4 = output driver off across the switch, on once clocks are stable
                                  // 8 = never move the DAC volume (no mute, no ramp): with any DC offset each
                                  //     volume step is itself a click
    int codecHold          = 1;   // 1 = keep the ES8311 analog stage powered between mic and speaker (no pop); 0 = M5Unified default
};

extern DianaTune Tune;
