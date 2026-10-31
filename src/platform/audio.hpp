// Audio output for ultramodern's audio_callbacks_t.
//
// The game's audio thread builds an audio command list each frame, the recompiled n_aspMain
// microcode (src/runtime/rsp.cpp) renders it into an RDRAM buffer, and osAiSetNextBuffer hands that
// buffer to queue_samples. The RT64 build plays it through SDL2; the null build paces it against
// the wall clock so the game's AI_LEN feedback (how many samples to render next frame) behaves as
// on hardware. Either build can record what the game queued with CONKER_DUMP_AUDIO=<file.wav>.
#pragma once

#include <cstddef>
#include <cstdint>

namespace conker::audio {

void init();
void queue_samples(int16_t* samples, size_t sample_count);
// While on, the game's sound is dropped instead of played (online, a rejoining machine catching up
// runs the game faster than real time); turning it on also drops what is queued.
void set_discard(bool discard);
size_t get_frames_remaining();
void set_frequency(uint32_t freq);

// The volume the game is played at, in tenths of full volume (0 is silent), set from the
// OPTIONS page (src/enhancements/display_options/display_options.cpp) and kept in
// conker-recomp-data/audio.cfg.
constexpr int kMaxVolume = 10;
int volume();
void set_volume(int volume);

} // namespace conker::audio
