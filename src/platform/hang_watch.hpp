// Notices the main thread stopping: it pumps the window's events every millisecond
// (src/platform/window_sdl.cpp), and when it stops the window stops answering and the game soon
// waits on it too. After kHangSeconds a line is printed and, on macOS, `sample` records every
// thread's stack for a few seconds into conker-recomp-data/debug_reports/hang-<date>-<time>.txt;
// when the main thread runs again, how long it was stuck. Freezes while switching between a window
// and full screen were seen this way only as macOS's "slow hid response" in the system log.
#pragma once

namespace conker::hang_watch {

// Starts the watching thread; call once from the main thread.
void start();

// The main thread is running; call every time round its loop.
void beat();

} // namespace conker::hang_watch
