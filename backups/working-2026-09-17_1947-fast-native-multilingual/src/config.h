// DIANA // Cardputer ADV edition — shared constants
#pragma once
#include <Arduino.h>

#ifndef DIANA_VERSION
#define DIANA_VERSION "1.0.0"
#endif

#define DIANA_ID        "D-I-0336-7"
#define DIANA_NAME      "DIANA"

// ── microSD (Cardputer / Cardputer ADV share the same SPI wiring) ──────────
#define SD_PIN_SCK      40
#define SD_PIN_MISO     39
#define SD_PIN_MOSI     14
#define SD_PIN_CS       12

// ── Files on the SD card ───────────────────────────────────────────────────
#define DIANA_DIR       "/diana"
#define CONFIG_PATH     "/diana/config.json"
#define MEMORY_PATH     "/diana/memory.json"
#define NOTES_PATH      "/diana/notes.txt"
#define LOG_PATH        "/diana/chatlog.txt"
#define HISTORY_PATH    "/diana/history.json"
#define IR_CODES_PATH   "/diana/ir_codes.json"
#define BOOT_WAV_PATH   "/diana/boot.wav"
#define REC_PATH        "/diana/.rec.pcm"
#define TTS_PATH        "/diana/.tts.pcm"

// ── Audio ──────────────────────────────────────────────────────────────────
#define REC_RATE            16000      // mic sample rate sent to Gemini (audio/L16;rate=16000)
#define REC_MAX_SECONDS     12
#define REC_CHUNK_SAMPLES   512        // 32 ms per chunk
#define PREROLL_CHUNKS      16         // ~0.5 s kept before speech so the wake word isn't clipped
#define DEFAULT_MIC_GAIN    32         // ES8311/M5 mic magnification (16 = library default; 48 was too hot -> noise read as speech)
#define FOLLOWUP_MS         15000      // after Diana replies, this long you can talk without saying "Diana"
#define TTS_RATE            24000      // Gemini TTS returns 24 kHz 16-bit mono PCM
#define PLAY_CHUNK_SAMPLES  1024
// Streaming playback: the M5 speaker holds only 2 queued slots, so big buffers = the
// only way to get a deep jitter cushion. 8000 samples ≈ 333 ms; 2 slots ≈ 666 ms buffered.
#define STREAM_CHUNK_SAMPLES 2400      // ~100 ms per buffer (smaller = sound starts sooner after first audio)
#define STREAM_PREBUFFER     1         // start after ~1 buffer so the reply begins sooner

// ── Conversation limits ────────────────────────────────────────────────────
#define HISTORY_MAX_TURNS   8          // exchanges kept in context (persisted across reboots)
#define HISTORY_MAX_CHARS   3600
#define HISTORY_PERSIST     12         // turns saved to SD so she remembers after a power cycle
#define MEMORY_MAX_CHARS    1400       // memory block injected into the system prompt
#define MEMORY_VALUE_MAX    280
#define REPLY_MAX_TOKENS    220        // short replies: fewer output tokens + shorter/faster TTS
#define HTTP_TIMEOUT_MS     25000

// ── Gemini ─────────────────────────────────────────────────────────────────
#define GEMINI_HOST         "generativelanguage.googleapis.com"
#define DEFAULT_CHAT_MODEL  "gemini-3.5-flash-lite"
#define DEFAULT_TTS_MODEL   "gemini-3.1-flash-tts-preview"   // low-latency, streams; 2.5 is quota-capped on free tier
#define DEFAULT_TTS_VOICE   "Leda"
#define DEFAULT_TTS_STYLE   "Say this in a soft, gentle, slightly peppy young voice: "

// ── Setup portal ───────────────────────────────────────────────────────────
#define SETUP_AP_SSID       "DIANA-SETUP"

// ── UI geometry (240x135 landscape) ────────────────────────────────────────
#define SCREEN_W        240
#define SCREEN_H        135
#define BAR_H           12
#define INPUT_H         19
#define LOG_Y           BAR_H
#define LOG_H           (SCREEN_H - BAR_H - INPUT_H)   // 104 px
#define INPUT_Y         (SCREEN_H - INPUT_H)
