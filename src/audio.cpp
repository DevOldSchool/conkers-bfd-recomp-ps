#include "audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

#if CONKER_AUDIO_SDL
#include <SDL.h>
#endif

namespace conker::audio {

namespace {

using Clock = std::chrono::steady_clock;

std::mutex audio_mutex;
uint32_t frequency = 48000;

// Conker's audio thread (func_800095A0) reads AI_LEN before queueing each frame and renders a
// shorter frame while at least 249 samples are still pending, so it keeps only ~11 ms buffered:
// enough for the AI's two-buffer DMA FIFO, not for a host audio device that pulls 512-frame
// periods. Reporting the queue minus this cushion makes the same feedback loop settle with the
// cushion extra buffered. CONKER_AUDIO_LATENCY_MS overrides it.
double latency_cushion_ms = 60.0;
std::vector<int16_t> frames;  // interleaved L, R scratch buffer

// --- statistics: a line every few seconds of audio so a headless log shows what was rendered --
uint64_t total_frames = 0;
uint64_t stats_frames = 0;
int stats_peak = 0;
double stats_square_sum = 0.0;
size_t stats_min_queued = SIZE_MAX;  // output queue depth (frames) seen when a buffer arrives
uint64_t stats_underruns = 0;         // buffers that arrived to an empty output queue

void note_queue_depth(size_t queued) {
    stats_min_queued = std::min(stats_min_queued, queued);
    if (queued == 0 && total_frames != 0) stats_underruns++;
}

void update_stats(const int16_t* data, size_t frame_count) {
    for (size_t i = 0; i < frame_count * 2; i++) {
        const int v = data[i];
        stats_peak = std::max(stats_peak, std::abs(v));
        stats_square_sum += static_cast<double>(v) * v;
    }
    stats_frames += frame_count;
    total_frames += frame_count;
    if (stats_frames >= static_cast<uint64_t>(frequency) * 5) {
        const double rms = std::sqrt(stats_square_sum / static_cast<double>(stats_frames * 2));
        std::printf("[conker] audio: %.1f s rendered at %u Hz, last 5 s peak %d rms %.0f, output queue min %.0f ms, %llu underruns\n",
                    static_cast<double>(total_frames) / frequency, frequency, stats_peak, rms,
                    1000.0 * static_cast<double>(stats_min_queued) / frequency,
                    static_cast<unsigned long long>(stats_underruns));
        stats_frames = 0;
        stats_peak = 0;
        stats_square_sum = 0.0;
        stats_min_queued = SIZE_MAX;
        stats_underruns = 0;
    }
}

// --- WAV dump (CONKER_DUMP_AUDIO) --------------------------------------------------------------
std::FILE* wav_file = nullptr;
uint64_t wav_frames = 0;

void put_u32(uint8_t* p, uint32_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

void put_u16(uint8_t* p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}

// Rewritten after every buffer so the file stays valid when the run is killed.
void write_wav_header() {
    uint8_t h[44] = {'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'A', 'V', 'E', 'f', 'm', 't', ' ',
                     0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 'd', 'a', 't', 'a'};
    const uint32_t data_bytes = static_cast<uint32_t>(wav_frames * 4);
    put_u32(h + 4, 36 + data_bytes);
    put_u32(h + 16, 16);              // fmt chunk size
    put_u16(h + 20, 1);               // PCM
    put_u16(h + 22, 2);               // channels
    put_u32(h + 24, frequency);       // sample rate
    put_u32(h + 28, frequency * 4);   // byte rate
    put_u16(h + 32, 4);               // block align
    put_u16(h + 34, 16);              // bits per sample
    put_u32(h + 40, data_bytes);
    std::fseek(wav_file, 0, SEEK_SET);
    std::fwrite(h, 1, sizeof(h), wav_file);
    std::fseek(wav_file, 0, SEEK_END);
}

void open_wav(const char* path) {
    wav_file = std::fopen(path, "wb");
    if (wav_file == nullptr) {
        std::perror("[conker] CONKER_DUMP_AUDIO");
        return;
    }
    write_wav_header();
    std::printf("[conker] writing audio to %s\n", path);
}

void write_wav(const int16_t* data, size_t frame_count) {
    if (wav_file == nullptr) return;
    std::vector<uint8_t> bytes(frame_count * 4);
    for (size_t i = 0; i < frame_count * 2; i++) {
        put_u16(bytes.data() + i * 2, static_cast<uint16_t>(data[i]));
    }
    std::fwrite(bytes.data(), 1, bytes.size(), wav_file);
    wav_frames += frame_count;
    write_wav_header();
    std::fflush(wav_file);
}

// --- wall-clock sink: consumes queued frames at the output rate without playing them ----------
double clock_queued_frames = 0.0;
Clock::time_point clock_last{};

void clock_advance() {
    const Clock::time_point now = Clock::now();
    if (clock_last != Clock::time_point{}) {
        const double elapsed = std::chrono::duration<double>(now - clock_last).count();
        clock_queued_frames = std::max(0.0, clock_queued_frames - elapsed * frequency);
    }
    clock_last = now;
}

// --- SDL2 output (RT64 build) -------------------------------------------------------------------
#if CONKER_AUDIO_SDL
bool sdl_enabled = false;
SDL_AudioDeviceID sdl_device = 0;
uint32_t sdl_frequency = 0;

void sdl_open() {
    if (sdl_device != 0) {
        SDL_CloseAudioDevice(sdl_device);
        sdl_device = 0;
    }
    SDL_AudioSpec want{};
    want.freq = static_cast<int>(frequency);
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    // No allowed changes: SDL converts to whatever the device wants, so queued sizes stay in
    // the game's format and rate.
    sdl_device = SDL_OpenAudioDevice(nullptr, 0, &want, nullptr, 0);
    if (sdl_device == 0) {
        std::fprintf(stderr, "[conker] SDL_OpenAudioDevice failed: %s; audio is muted\n", SDL_GetError());
        sdl_enabled = false;
        return;
    }
    sdl_frequency = frequency;
    SDL_PauseAudioDevice(sdl_device, 0);
    std::printf("[conker] SDL audio output at %u Hz\n", frequency);
}
#endif

bool use_clock_sink() {
#if CONKER_AUDIO_SDL
    return !sdl_enabled;
#else
    return true;
#endif
}

} // namespace

void init() {
    if (const char* value = std::getenv("CONKER_AUDIO_LATENCY_MS"); value != nullptr && value[0] != '\0') {
        latency_cushion_ms = std::max(0.0, std::atof(value));
    }
    if (const char* path = std::getenv("CONKER_DUMP_AUDIO"); path != nullptr && path[0] != '\0') {
        open_wav(path);
    }
#if CONKER_AUDIO_SDL
    const char* setting = std::getenv("CONKER_AUDIO");
    if (setting != nullptr && setting[0] == '0') {
        std::printf("[conker] CONKER_AUDIO=0: SDL audio output disabled\n");
    } else if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        std::fprintf(stderr, "[conker] SDL audio init failed: %s; audio is muted\n", SDL_GetError());
    } else {
        sdl_enabled = true;
    }
#endif
}

void set_frequency(uint32_t freq) {
    std::lock_guard lock{audio_mutex};
    if (freq == 0 || freq == frequency) return;
    std::printf("[conker] audio frequency %u Hz\n", freq);
    clock_advance();
    frequency = freq;
    if (wav_file != nullptr && wav_frames != 0) {
        std::fprintf(stderr, "[conker] audio frequency changed after %llu frames; the WAV header uses the new rate\n",
                     static_cast<unsigned long long>(wav_frames));
    }
#if CONKER_AUDIO_SDL
    if (sdl_enabled && sdl_device != 0) sdl_open();
#endif
}

void queue_samples(int16_t* samples, size_t sample_count) {
    std::lock_guard lock{audio_mutex};
    const size_t frame_count = sample_count / 2;
    // The buffer is in RDRAM, which holds 32-bit words in host order: each word is one stereo
    // frame with the left sample in its high half, so the host's int16 view is R, L.
    frames.resize(frame_count * 2);
    for (size_t i = 0; i < frame_count; i++) {
        frames[i * 2 + 0] = samples[i * 2 + 1];
        frames[i * 2 + 1] = samples[i * 2 + 0];
    }
#if CONKER_AUDIO_SDL
    if (sdl_enabled && sdl_device != 0) {
        note_queue_depth(SDL_GetQueuedAudioSize(sdl_device) / 4);
    } else
#endif
    {
        clock_advance();
        note_queue_depth(static_cast<size_t>(clock_queued_frames));
    }
    update_stats(frames.data(), frame_count);
    write_wav(frames.data(), frame_count);
#if CONKER_AUDIO_SDL
    if (sdl_enabled) {
        if (sdl_device == 0 || sdl_frequency != frequency) sdl_open();
        if (sdl_device != 0) {
            SDL_QueueAudio(sdl_device, frames.data(), static_cast<Uint32>(frame_count * 4));
            return;
        }
    }
#endif
    clock_advance();
    clock_queued_frames += static_cast<double>(frame_count);
}

size_t get_frames_remaining() {
    std::lock_guard lock{audio_mutex};
    size_t queued;
#if CONKER_AUDIO_SDL
    if (!use_clock_sink() && sdl_device != 0) {
        queued = SDL_GetQueuedAudioSize(sdl_device) / 4;
    } else
#endif
    {
        clock_advance();
        queued = static_cast<size_t>(clock_queued_frames);
    }
    const size_t cushion = static_cast<size_t>(latency_cushion_ms * frequency / 1000.0);
    return queued > cushion ? queued - cushion : 0;
}

} // namespace conker::audio
