// CONKER_LATENCY_LOG=<file>: how long after the game samples the pads each picture made from that
// sample reaches the screen. A line per presented picture (see latency.cpp).
#pragma once

#include <cstdint>

namespace conker::latency {

// The game thread has taken this frame's input (conker_frame_input_begin).
void input_sampled();

// The game thread has queued the frame's graphics task (func_1501C880): it carries the last sample.
void task_queued();

// The renderer is about to draw a graphics task: the sample time (steady clock, ns) of the frame it
// belongs to, or 0 when unknown. RT64 carries it with the workload to the present.
uint64_t take_task_sample();

// RT64 has presented picture index of count made from the workload tagged with sample.
void presented(uint64_t sample, int index, int count);

bool enabled();

// Pictures presented so far (with frame interpolation, several a game frame).
uint64_t presents();

}  // namespace conker::latency
