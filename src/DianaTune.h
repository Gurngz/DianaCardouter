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
};

extern DianaTune Tune;
