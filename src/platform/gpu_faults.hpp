// GPU faults: command buffers the graphics driver fails, logged with what the game and the renderer
// were doing. See src/platform/gpu_faults.cpp.
#pragma once

#include <cstdint>
#include <string>

#include "json/json.hpp"

namespace conker::gpu_faults {

// Starts recording: `rdram` is read for the scene when a fault is logged.
void start(const uint8_t* rdram);

// A command buffer the driver failed, on any thread: the queue it was sent on (RT64's worker), the
// driver's error code and description, and the encoders that faulted.
void report(const char* queue, int64_t code, const char* description, const char* encoders);

// Something the renderer did that frees or remakes GPU resources (texture packs swapped, a new
// scale or shape, frame buffers discarded), kept with its VI to print with the next fault.
void note(const std::string& event);

// The faults so far, for a debug report.
nlohmann::json status();

// True once after the first fault: the game thread then makes a debug report, or puts the request
// back while it cannot.
bool take_report_request();
void request_report_again();

} // namespace conker::gpu_faults
