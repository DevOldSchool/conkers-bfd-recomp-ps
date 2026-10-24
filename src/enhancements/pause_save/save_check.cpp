// Save/load checks, driven by the environment (tools/save_check.py runs them over the debug reports):
// CONKER_SAVE_CHECK=<report folder or its save.bin> starts that report's save a few seconds after
// boot, as LOAD REPORT in the debug menu does, and once the restore has put the scene back
// (resume_point.cpp, restore_scene), writes the scene as it is then, and again after each of the
// times in CONKER_SAVE_CHECK_AT (seconds after the restore, "0,6,30" unless set; 0 is the restore's
// own frame, before the game has run on; capture-pre.resume is the scene as the game loaded it,
// before the restore), to CONKER_SAVE_CHECK_OUT (save-check/ under the working
// directory unless set) as capture-<seconds>.resume, in the points file's format (see
// write_capture). With CONKER_SAVE_CHECK_REPORT=1 a debug report is then made, so the save the game
// writes from the restored scene can be compared with the one it was loaded from. result.json says
// how it ended, and the game quits. CONKER_SAVE_CHECK_CHECKPOINT=1 starts the save at its checkpoint
// instead (as the game itself would), for a baseline of what the scene is like without the restore.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "build_version.h"
#include "json/json.hpp"
#include "recomp.h"
#include "enhancements/pause_save/resume_point.hpp"

namespace conker::resume::check {

void restoring_hook(uint8_t* rdram);
void restored_hook(uint8_t* rdram);
void count_frame();

namespace {

constexpr int kLoadFrame = 300;                // frames after boot: the title menu is up (CONKER_SKIP_INTRO=1)
constexpr int kFramesPerSecond = 30;
constexpr std::chrono::seconds kLoadLimit{120};  // restore not reached by then: timed out
constexpr std::chrono::seconds kReportLimit{20};

enum class State { Off, Waiting, Loading, Restored, Reporting, Done };
State g_state = State::Off;
int g_frames = 0;
int g_frames_since_restore = -1;
std::filesystem::path g_report;
std::filesystem::path g_out;
std::vector<int> g_at;      // seconds after the restore to capture at, in order
size_t g_next = 0;          // the next of g_at
bool g_make_report = false;
bool g_at_checkpoint = false;
std::string g_report_name;  // the debug report being made
std::chrono::steady_clock::time_point g_started;

void finish(const char* status, const std::string& detail, int code) {
    std::error_code error;
    std::filesystem::create_directories(g_out, error);
    nlohmann::json result = {{"status", status}, {"detail", detail}, {"captures", nlohmann::json::array()}};
    for (size_t i = 0; i < g_next && i < g_at.size(); i++) {
        result["captures"].push_back(g_at[i]);
    }
    if (!g_report_name.empty()) {
        result["report"] = g_report_name;
    }
    std::ofstream(g_out / "result.json", std::ios::trunc) << result.dump(1) << '\n';
    std::printf("[conker] save check: %s%s%s\n", status, detail.empty() ? "" : ", ", detail.c_str());
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(code);
}

// The report SAVE DEBUG REPORT makes, with what check_report needs (the debug menu's has more).
std::string report_json(uint8_t* rdram) {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char when[32];
    std::strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &local);
    nlohmann::json report = {{"format", "conker-recomp-debug-report"}, {"version", 1}, {"made", when},
                             {"build", CONKER_BUILD_VERSION},          {"save_check", true}};
    Position p;
    if (current_position(rdram, &p)) {
        report["scene"] = p.scene;
        report["entrance"] = 0;
        report["conker"] = {{"x", p.x}, {"y", p.y}, {"z", p.z}, {"facing", p.facing}, {"angle", p.yaw}};
    }
    return report.dump();
}

void capture(uint8_t* rdram, const std::string& name) {
    std::error_code error;
    std::filesystem::create_directories(g_out, error);
    const std::filesystem::path path = g_out / ("capture-" + name + ".resume");
    if (write_capture(rdram, path)) {
        std::printf("[conker] save check: capture %s, %s\n", name.c_str(), path.string().c_str());
    } else {
        std::printf("[conker] save check: no Conker to capture %s\n", name.c_str());
    }
}

void capture(uint8_t* rdram, int seconds) { capture(rdram, std::to_string(seconds)); }

void start() {
    const char* report = std::getenv("CONKER_SAVE_CHECK");
    if (report == nullptr || report[0] == '\0') {
        return;
    }
    g_report = report;
    if (std::filesystem::is_directory(g_report)) {
        g_report /= "save.bin";
    }
    const char* out = std::getenv("CONKER_SAVE_CHECK_OUT");
    g_out = out != nullptr && out[0] != '\0' ? std::filesystem::path(out) : std::filesystem::path("save-check");
    const char* at = std::getenv("CONKER_SAVE_CHECK_AT");
    for (const char* p = at != nullptr && at[0] != '\0' ? at : "0,6,30"; *p != '\0';) {
        char* end;
        const long seconds = std::strtol(p, &end, 10);
        if (end == p) {
            break;
        }
        g_at.push_back(static_cast<int>(seconds));
        p = *end == ',' ? end + 1 : end;
    }
    const char* make = std::getenv("CONKER_SAVE_CHECK_REPORT");
    g_make_report = make != nullptr && make[0] == '1';
    const char* checkpoint = std::getenv("CONKER_SAVE_CHECK_CHECKPOINT");
    g_at_checkpoint = checkpoint != nullptr && checkpoint[0] == '1';
    g_state = State::Waiting;
    std::printf("[conker] save check: %s, captures at %zu times, output %s\n", g_report.string().c_str(), g_at.size(),
                g_out.string().c_str());
}

}  // namespace

// The scene as the game loaded it, before the restore: what a field goes back to when the game
// undoes the restore (capture-pre.resume).
void restoring_hook(uint8_t* rdram) {
    if (g_state == State::Loading) {
        capture(rdram, "pre");
    }
}

void restored_hook(uint8_t* rdram) {
    if (g_state != State::Loading) {
        return;
    }
    g_state = State::Restored;
    g_frames_since_restore = 0;
    while (g_next < g_at.size() && g_at[g_next] <= 0) {
        capture(rdram, g_at[g_next++]);
    }
}

void tick(uint8_t* rdram, recomp_context* /*ctx*/) {
    static bool started = false;
    if (!started) {
        started = true;
        start();
    }
    g_frames++;
    count_frame();
    switch (g_state) {
        case State::Off:
        case State::Done:
            return;
        case State::Waiting: {
            if (g_frames < kLoadFrame) {
                return;
            }
            std::string problem;
            if (!load_report(rdram, g_report, &problem, g_at_checkpoint)) {
                if (problem == "saving, try again") {
                    return;
                }
                finish("refused", problem, 3);
            }
            g_state = State::Loading;
            g_started = std::chrono::steady_clock::now();
            return;
        }
        case State::Loading:
            if (std::chrono::steady_clock::now() - g_started > kLoadLimit) {
                finish("timeout", "the scene was not restored", 4);
            }
            return;
        case State::Restored:
            g_frames_since_restore++;
            while (g_next < g_at.size() && g_frames_since_restore >= g_at[g_next] * kFramesPerSecond) {
                capture(rdram, g_at[g_next++]);
            }
            if (g_next < g_at.size()) {
                return;
            }
            if (!g_make_report) {
                finish("done", "", 0);
            }
            g_report_name = request_report(rdram, report_json(rdram));
            if (g_report_name.empty()) {
                return;  // a save is being written; asked again next frame
            }
            g_state = State::Reporting;
            g_started = std::chrono::steady_clock::now();
            return;
        case State::Reporting: {
            const std::filesystem::path folder = data_folder() / "debug_reports" / g_report_name;
            std::error_code error;
            if (std::filesystem::exists(folder / "report.json", error)) {
                finish("done", "", 0);
            }
            if (std::chrono::steady_clock::now() - g_started > kReportLimit) {
                finish("timeout", "the debug report was not made", 5);
            }
            return;
        }
    }
}

}  // namespace conker::resume::check
