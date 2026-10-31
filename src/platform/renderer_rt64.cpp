// RT64 renderer context for the runtime (CONKER_RENDERER=rt64).
//
// RT64 interprets the game's display lists in HLE; Conker's F3DEX2CBFD microcode is supported
// through tools/patches/rt64-f3dex2cbfd.patch. The context runs on ultramodern's gfx thread, the
// window it draws into is created on the main thread (src/platform/window_sdl.cpp).
//
// RT64 renders at the whole multiple of the game's picture that covers the window height (its
// WindowIntegerScale default), which follows the window when it is resized or made full screen.
// Instead it renders at the multiple chosen in OPTIONS for the mode (src/platform/display_mode.hpp)
// and scales it to fill the window or screen, or presents it at its own size, centered
// (presentation.nativeSize, tools/patches/rt64-native-size.patch). A window opens at its multiple,
// so the two differ there only once it is resized by hand.
//
// In widescreen RT64 expands the picture to the window's or screen's shape (AspectRatio::Expand): it
// widens the game's perspective projections and the frame buffer, so more of the scene shows at the
// sides rather than the picture being stretched. 2D rectangles stay centered in the 4:3 area unless
// they span the whole width, so the HUD and menus keep their place and full-screen fills reach the
// edges. An untextured rectangle that reaches only one side, like the black around the circle wipe
// that opens and closes a scene, is stretched to that side of the screen (rect.extendEdgeRects,
// tools/patches/rt64-edge-rects.patch). The pause menu shows the paused scene at its full width,
// without the game's blur (src/enhancements/display_options/widescreen_copies.cpp). The game still culls to its own 4:3
// view, so an object can appear late at the far sides.
//
// Texture packs in RT64's replacement format (a folder, .rtz or .zip with an rt64.json that maps
// texture hashes to images) are loaded from conker-recomp-data/texture_packs under the working
// directory, in name order; where two packs replace the same texture the later name wins.
// CONKER_TEXTURE_PACKS=0 loads none. Before them come the built-in sharper font and menus, made from
// the ROM (src/enhancements/sharp_font/sharp_font.cpp, src/enhancements/sharp_menus/sharp_menus.cpp),
// so a pack can replace them. They load with the first graphics task, once the game has loaded the
// ROM. With an Xbox or PlayStation pad, its button pictures come after them
// (src/enhancements/pad_icons/pad_icons.cpp), whatever the OPTIONS menu's text choice; the packs are
// handed over again when the pad changes. CONKER_DUMP_TEXTURES=<dir> writes each texture the game
// loads to <dir> once, as RT64's <hash>.v5.png, .tmem and .tile.json files, to start a pack from.
// With any of them loaded, the rectangles the game draws text glyphs with are widened so a replaced
// glyph is not cut off (src/platform/font_rects.cpp).
//
// Command buffers the GPU fails are logged with the RT64 worker that sent them and the renderer's
// recent resource changes, noted here (src/platform/gpu_faults.cpp).
#if defined(_WIN32)
// Before ultramodern's headers include a lean windows.h: RT64's D3D12 and DXC headers need the COM
// types it leaves out.
#include <windows.h>
#endif
#include "platform/renderer_rt64.hpp"
#include "platform/render_debug.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "enhancements/pad_icons/pad_icons.hpp"
#include "enhancements/sharp_font/sharp_font.hpp"
#include "enhancements/sharp_menus/sharp_menus.hpp"
#include "fixes/frame_effects.hpp"
#include "gbi/rt64_gbi.h"
#include "hle/rt64_application.h"
#include "hle/rt64_interpreter.h"
#include "hle/rt64_state.h"
#include "librecomp/game.hpp"
#include "ultramodern/error_handling.hpp"
#include "platform/font_rects.hpp"
#include "platform/gpu_faults.hpp"
#include "platform/input.hpp"
#include "platform/lobby_ui.hpp"
#include "platform/object_picker_rt64.hpp"
#include "render/rt64_texture_cache.h"
#include "rhi/rt64_render_hooks.h"
#include "runtime/frame_input.hpp"
#include "runtime/net_play.hpp"
#include "runtime/snapshot.hpp"
#include "platform/display_mode.hpp"
#include "plume_render_interface.h"

namespace conker::renderer {

namespace {

uint8_t dmem[0x1000];
uint8_t imem[0x1000];
uint32_t mi_intr_reg = 0;
uint32_t dpc_start_reg = 0;
uint32_t dpc_end_reg = 0;
uint32_t dpc_current_reg = 0;
uint32_t dpc_status_reg = 0;
uint32_t dpc_clock_reg = 0;
uint32_t dpc_bufbusy_reg = 0;
uint32_t dpc_pipebusy_reg = 0;
uint32_t dpc_tmem_reg = 0;
unsigned char dummy_rom_header[0x40];

void dummy_check_interrupts() {}

// CONKER_RENDER_SELFTEST=<task>:<kind>: at that graphics task, a display list or task of the kind a
// hostile host's snapshot could leave in game memory is drawn, and RT64 must refuse it:
//   ucode     the task names microcode that is not Conker's: the game stops (Q2)
//   vertex    vertices, a full sync, then a modify and a triangle on the old slots: both dropped (Q1)
//   extended  the extended GBI's RDRAM-extending and vertex-segment commands: ignored (Q5)
//   dlstack   a list that calls itself: stopped at the depth limit, nothing left behind (Q6)
//   loadtile  a texture load with its rows reversed: skipped at once (Q7)
// The lists are written to RDRAM above the game's 4 MiB, which it does not use.
struct RenderSelfTest {
    uint64_t task = 0;
    std::string kind;
};

const RenderSelfTest& render_self_test() {
    static const RenderSelfTest test = [] {
        RenderSelfTest t;
        const char* spec = std::getenv("CONKER_RENDER_SELFTEST");
        char* end = nullptr;
        if (spec != nullptr) {
            t.task = std::strtoull(spec, &end, 0);
            if (*end == ':') {
                t.kind = end + 1;
            }
        }
        return t;
    }();
    return test;
}

constexpr uint32_t kTestList = 0x80600000;     // the lists
constexpr uint32_t kTestData = 0x80610000;     // vertices and texels (zero)
constexpr uint32_t kTestZeroes = 0x80620000;   // not any microcode RT64 knows

struct TestList {
    uint8_t* rdram;
    uint32_t at = kTestList;
    void command(uint32_t w0, uint32_t w1) {
        std::memcpy(rdram + (at - 0x80000000u), &w0, 4);  // RDRAM holds host-order 32-bit words
        std::memcpy(rdram + (at - 0x80000000u) + 4, &w1, 4);
        at += 8;
    }
};

void report_self_test(const char* kind, bool pass, const std::string& detail) {
    std::printf("[conker] render self-test (%s): %s, %s\n", kind, pass ? "PASS" : "FAIL", detail.c_str());
    std::fflush(stdout);
}

// Runs the self-test's list after the game's for the task (the RDP state is then the game's).
void run_render_self_test(RT64::Application& app, const std::string& kind) {
    uint8_t* rdram = app.core.RDRAM;
    std::memset(rdram + (kTestData - 0x80000000u), 0, 0x10000);
    TestList list{rdram};
    RT64::State& state = *app.state;
    RT64::RSP& rsp = *state.rsp;
    if (kind == "vertex") {
        for (int i = 0; i < 64; i++) {
            list.command(0x01010020u, kTestData);  // G_VTX: 16 vertices into slots 0-15
        }
        list.command(0xE9000000u, 0);              // G_RDPFULLSYNC: the workload moves on
        list.command(0x02100000u, 0xDEADBEEFu);    // G_MODIFYVTX: slot 0's colour
        list.command(0x05000204u, 0);              // G_TRI1: slots 0, 1, 2
        list.command(0xE9000000u, 0);
        list.command(0xDF000000u, 0);              // G_ENDDL
        const uint32_t dropped = rsp.droppedVertexCommands;
        app.processDisplayLists(rdram, kTestList & 0x3FFFFFF, 0, true);
        const uint32_t now = rsp.droppedVertexCommands - dropped;
        report_self_test("vertex", now >= 2, std::to_string(now) + " commands on old vertex slots dropped");
    }
    if (kind == "extended") {
        list.command(0xE0525464u, 0x10000064u);    // G_SPNOOP hook: enable the extended GBI as 0x64
        list.command(0x6400002Cu, 1);              // G_EX_SETRDRAMEXTENDED_V1 on
        list.command(0x64000031u, 1u | (15u << 1)); // G_EX_SETVERTEXSEGMENT_V1: element 15 on
        list.command(0xFFFFFFF0u, 0);
        list.command(0x01010020u, 0xC0000000u);    // G_VTX from an extended address (1 GiB in)
        list.command(0xE0525464u, 0x20000000u);    // disable the extended GBI
        list.command(0xE9000000u, 0);
        list.command(0xDF000000u, 0);
        app.processDisplayLists(rdram, kTestList & 0x3FFFFFF, 0, true);
        bool segments = false;
        for (bool on : rsp.extended.vertexSegmentEnabled) {
            segments = segments || on;
        }
        report_self_test("extended", !state.extended.extendRDRAM && !segments,
                         std::string("extended RDRAM ") + (state.extended.extendRDRAM ? "on" : "off") + ", vertex segments " +
                             (segments ? "on" : "off"));
    }
    if (kind == "dlstack") {
        list.command(0xDE000000u, kTestList & 0x3FFFFFF);  // G_DL (called): itself
        list.command(0xDF000000u, 0);
        app.processDisplayLists(rdram, kTestList & 0x3FFFFFF, 0, true);
        const size_t depth = state.returnAddressStack.size();
        report_self_test("dlstack", depth <= RT64::State::ReturnAddressStackLimit,
                         std::to_string(depth) + " return addresses left after the list");
    }
    if (kind == "loadtile") {
        list.command(0xFD100000u, kTestData);      // G_SETTIMG: RGBA16, 1 texel wide
        list.command(0xF5100000u, 0x07000000u);    // G_SETTILE: load tile, line 0
        list.command(0xF4000400u, 0x0703C000u);    // G_LOADTILE: rows 256 down to 0
        list.command(0xE9000000u, 0);
        list.command(0xDF000000u, 0);
        const auto start = std::chrono::steady_clock::now();
        app.processDisplayLists(rdram, kTestList & 0x3FFFFFF, 0, true);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        report_self_test("loadtile", ms < 1000.0, "the list took " + std::to_string(static_cast<int>(ms)) + " ms");
    }
}

// Graphics tasks run Conker's own microcode (F3DEX2CBFD) only. The task's microcode pointer is game
// data, and RT64 picks its command handlers by the bytes there, so any other microcode, or none, comes
// from memory this run cannot trust.
void require_conker_microcode(RT64::Application& app, const OSTask* task) {
    const RT64::GBI* gbi = app.interpreter->hleGBI;
    if (gbi == nullptr || gbi->ucode != RT64::GBIUCode::F3DEX2CBFD) {
        std::fprintf(stderr, "[conker] The graphics task names microcode at 0x%08X that is not the game's own: the game's "
                             "memory is not one this run can trust. Stopping.\n",
                     static_cast<uint32_t>(task->t.ucode));
        ULTRAMODERN_QUICK_EXIT();
    }
}

std::mutex g_texture_packs_mutex;
std::vector<std::string> g_texture_packs;  // in use, for texture_packs()
std::string g_gpu_name;                    // after setup, for gpu_name() and graphics_api()
std::string g_graphics_api;

// The built-in packs, made once from the ROM, and the user's packs.
struct Packs {
    std::shared_ptr<RT64::FileSystem> font;
    std::shared_ptr<RT64::FileSystem> menus;
    std::shared_ptr<RT64::FileSystem> buttons[conker::pad_icons::kStyles][2];  // by Style and camera swap, made when first wanted
    std::vector<std::filesystem::path> user;
};

Packs find_packs() {
    Packs packs;
    if (conker::sharp_font::enabled()) {
        packs.font = conker::sharp_font::make_pack(recomp::get_rom());
        if (packs.font) conker::sharp_font::dump_pack(*packs.font);
    }
    if (conker::sharp_menus::enabled()) {
        packs.menus = conker::sharp_menus::make_pack();
        if (packs.menus) conker::sharp_menus::dump_pack(*packs.menus);
    }
    const char* setting = std::getenv("CONKER_TEXTURE_PACKS");
    if (setting == nullptr || std::string(setting) != "0") {
        const std::filesystem::path dir = std::filesystem::current_path() / "conker-recomp-data" / "texture_packs";
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
            const std::string name = entry.path().filename().string();
            std::string extension = entry.path().extension().string();
            std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
            if (!name.starts_with(".") &&
                (entry.is_directory() || extension == ".rtz" || extension == ".zip")) {
                packs.user.push_back(entry.path());
            }
        }
        std::sort(packs.user.begin(), packs.user.end());
    }
    return packs;
}

// Hands RT64 the packs to use, the built-in font and menus only if `sharp` (the OPTIONS menu's
// choice) and `pad`'s button pictures (the redrawn N64 ones also only if `sharp`; an Xbox or
// PlayStation pad's and the keyboard's always, as they say which button to press), and returns whether the font's
// glyphs may be replaced: the built-in font or any user pack is in.
bool use_packs(RT64::Application& app, Packs& packs, bool sharp, conker::pad_icons::Style pad, bool swapped, bool report) {
    std::vector<RT64::ReplacementDirectory> directories;
    std::vector<std::string> names;
    if (sharp && packs.font) {
        directories.emplace_back(packs.font, "built-in sharp font");
        names.push_back("built-in sharp font");
    }
    if (sharp && packs.menus) {
        directories.emplace_back(packs.menus, "built-in sharp menus");
        names.push_back("built-in sharp menus");
    }
    if (pad == conker::pad_icons::Style::N64 && !sharp) pad = conker::pad_icons::Style::Original;
    // The N64 set shows the C buttons themselves, so the camera swap does not change it.
    if (pad == conker::pad_icons::Style::N64) swapped = true;
    std::shared_ptr<RT64::FileSystem>& buttons = packs.buttons[int(pad)][swapped];
    if (!buttons) buttons = conker::pad_icons::make_pack(pad, swapped);
    if (buttons) {
        const std::string name = std::string("built-in ") + conker::pad_icons::name(pad) + " buttons";
        directories.emplace_back(buttons, name);
        names.push_back(name);
    }
    for (const std::filesystem::path& pack : packs.user) {
        directories.emplace_back(pack);
        names.push_back("texture pack " + pack.filename().string());
    }
    if (directories.empty()) {
        app.textureCache->clearReplacementDirectories();
        conker::gpu_faults::note("texture packs cleared");
        std::lock_guard lock(g_texture_packs_mutex);
        g_texture_packs.clear();
        return false;
    }
    const bool loaded = app.textureCache->loadReplacementDirectories(directories);
    {
        std::string event = "texture packs replaced:";
        for (const std::string& name : names) {
            event += (event.back() == ':' ? " " : ", ") + name;
        }
        conker::gpu_faults::note(event + (loaded ? "" : " (not loaded)"));
    }
    if (report) {
        for (const std::string& name : names) {
            std::printf("[conker] %s%s\n", name.c_str(), loaded ? "" : " (not loaded)");
        }
    }
    {
        std::lock_guard lock(g_texture_packs_mutex);
        g_texture_packs = loaded ? names : std::vector<std::string>();
    }
    return loaded && ((sharp && packs.font) || !packs.user.empty());
}

void start_texture_dump(RT64::Application& app) {
    if (const char* path = std::getenv("CONKER_DUMP_TEXTURES"); path != nullptr && path[0] != '\0') {
        std::error_code error;
        std::filesystem::create_directories(path, error);
        app.state->dumpingTexturesDirectory = path;
        std::printf("[conker] dumping textures to %s\n", path);
    }
}

ultramodern::renderer::SetupResult map_setup_result(RT64::Application::SetupResult result) {
    switch (result) {
        case RT64::Application::SetupResult::Success: return ultramodern::renderer::SetupResult::Success;
        case RT64::Application::SetupResult::DynamicLibrariesNotFound: return ultramodern::renderer::SetupResult::DynamicLibrariesNotFound;
        case RT64::Application::SetupResult::InvalidGraphicsAPI: return ultramodern::renderer::SetupResult::InvalidGraphicsAPI;
        case RT64::Application::SetupResult::GraphicsAPINotFound: return ultramodern::renderer::SetupResult::GraphicsAPINotFound;
        case RT64::Application::SetupResult::GraphicsDeviceNotFound: return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
    }
    return ultramodern::renderer::SetupResult::GraphicsDeviceNotFound;
}

class Rt64Renderer final : public ultramodern::renderer::RendererContext {
public:
    Rt64Renderer(uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
        RT64::Application::Core core{};
#if defined(_WIN32)
        core.window = window_handle.window;
#elif defined(__APPLE__)
        core.window.window = window_handle.window;
        core.window.view = window_handle.view;
#else
        core.window = window_handle;
#endif
        core.checkInterrupts = dummy_check_interrupts;
        core.HEADER = dummy_rom_header;
        core.RDRAM = rdram;
        core.DMEM = dmem;
        core.IMEM = imem;
        core.MI_INTR_REG = &mi_intr_reg;
        core.DPC_START_REG = &dpc_start_reg;
        core.DPC_END_REG = &dpc_end_reg;
        core.DPC_CURRENT_REG = &dpc_current_reg;
        core.DPC_STATUS_REG = &dpc_status_reg;
        core.DPC_CLOCK_REG = &dpc_clock_reg;
        core.DPC_BUFBUSY_REG = &dpc_bufbusy_reg;
        core.DPC_PIPEBUSY_REG = &dpc_pipebusy_reg;
        core.DPC_TMEM_REG = &dpc_tmem_reg;

        ultramodern::renderer::ViRegs* vi = ultramodern::renderer::get_vi_regs();
        core.VI_STATUS_REG = &vi->VI_STATUS_REG;
        core.VI_ORIGIN_REG = &vi->VI_ORIGIN_REG;
        core.VI_WIDTH_REG = &vi->VI_WIDTH_REG;
        core.VI_INTR_REG = &vi->VI_INTR_REG;
        core.VI_V_CURRENT_LINE_REG = &vi->VI_V_CURRENT_LINE_REG;
        core.VI_TIMING_REG = &vi->VI_TIMING_REG;
        core.VI_V_SYNC_REG = &vi->VI_V_SYNC_REG;
        core.VI_H_SYNC_REG = &vi->VI_H_SYNC_REG;
        core.VI_LEAP_REG = &vi->VI_LEAP_REG;
        core.VI_H_START_REG = &vi->VI_H_START_REG;
        core.VI_V_START_REG = &vi->VI_V_START_REG;
        core.VI_V_BURST_REG = &vi->VI_V_BURST_REG;
        core.VI_X_SCALE_REG = &vi->VI_X_SCALE_REG;
        core.VI_Y_SCALE_REG = &vi->VI_Y_SCALE_REG;

        conker::gpu_faults::start(rdram);
        plume::renderCommandErrorCallback().store([](const plume::RenderCommandError& error) {
            conker::gpu_faults::report(error.queueName, error.code, error.description, error.faultedEncoders);
        });

        RT64::ApplicationConfiguration app_config;
        app_config.useConfigurationFile = false;
        app_ = std::make_unique<RT64::Application>(core, app_config);

#if defined(__APPLE__)
        app_->userConfig.graphicsAPI = RT64::UserConfiguration::GraphicsAPI::Metal;
#endif
        app_->userConfig.developerMode = developer_mode;
        app_->userConfig.resolution = RT64::UserConfiguration::Resolution::WindowIntegerScale;

        uint32_t thread_id = 0;
#if defined(_WIN32)
        thread_id = window_handle.thread_id;
#endif
        setup_result = map_setup_result(app_->setup(thread_id));
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        if (setup_result != ultramodern::renderer::SetupResult::Success) {
            std::fprintf(stderr, "[conker] RT64 setup failed (%d)\n", static_cast<int>(setup_result));
            app_ = nullptr;
            return;
        }
        start_texture_dump(*app_);
        conker::lobby_ui::attach(app_.get());
        install_object_picker();
        conker::frame_effects::use_gpu(true);
        {
            std::lock_guard lock(g_texture_packs_mutex);
            g_gpu_name = app_->device->getDescription().name;
            switch (app_->chosenGraphicsAPI) {
                case RT64::UserConfiguration::GraphicsAPI::D3D12: g_graphics_api = "D3D12"; break;
                case RT64::UserConfiguration::GraphicsAPI::Vulkan: g_graphics_api = "Vulkan"; break;
                case RT64::UserConfiguration::GraphicsAPI::Metal: g_graphics_api = "Metal"; break;
                default: g_graphics_api = "unknown"; break;
            }
        }
    }

    ~Rt64Renderer() override = default;

    bool valid() override { return app_ != nullptr; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& /*old_config*/,
                       const ultramodern::renderer::GraphicsConfig& /*new_config*/) override {
        return true;
    }

    void enable_instant_present() override {
        app_->enhancementConfig.presentation.mode = RT64::EnhancementConfiguration::Presentation::Mode::PresentEarly;
        app_->updateEnhancementConfig();
    }

    void send_dl(const OSTask* task) override {
        // Online, nothing drawn is written back into game memory on any machine: each GPU draws a little
        // differently, and the game reads depth back for the camera (func_1512B1B8), so the games parted
        // at once between two Macs (rt64-online-writeback.patch). The frame buffers above stay written.
        app_->state->protectedRDRAMEnd.store(conker::net::game_memory_end(), std::memory_order_relaxed);
        if (const uint32_t skipped = app_->state->protectedWritesSkipped.load(std::memory_order_relaxed);
            skipped != 0 && reported_skips_ == 0) {
            reported_skips_ = skipped;
            std::printf("[conker] net: the renderer keeps what it draws out of game memory (first at 0x%08X)\n",
                        0x80000000u | app_->state->protectedLastSkipped.load(std::memory_order_relaxed));
        }
        if (conker::snapshot::take_framebuffer_discard()) {
            app_->forgetFramebuffers();
        }
        // Whether the game drew a frame copy for this task (see apply_display_scale).
        const uint32_t copies = conker::display::frame_copy_count();
        tasks_without_copy_ = copies != seen_copies_ ? 0 : tasks_without_copy_ + 1;
        seen_copies_ = copies;
        if (!packs_loaded_) {
            packs_loaded_ = true;
            packs_ = find_packs();
            used_sharp_ = conker::display::sharp();
            used_pad_ = conker::pad_icons::style();
            used_swap_ = conker::input::camera_swapped();
            widen_font_rects_ = use_packs(*app_, packs_, used_sharp_, used_pad_, used_swap_, true);
        } else {
            const bool sharp = conker::display::sharp();
            const conker::pad_icons::Style pad = conker::pad_icons::style();
            const bool swap = conker::input::camera_swapped();
            if (sharp != used_sharp_ || pad != used_pad_ || swap != used_swap_) {
                const bool new_pad = pad != used_pad_;
                used_sharp_ = sharp;
                used_pad_ = pad;
                used_swap_ = swap;
                widen_font_rects_ = use_packs(*app_, packs_, used_sharp_, used_pad_, used_swap_, new_pad);
            }
        }
        if (widen_font_rects_) {
            widen_font_rects(app_->core.RDRAM, task->t.data_ptr);
        }
        update_object_picker();
        const RenderSelfTest& self_test = render_self_test();
        const bool self_test_now = !self_test.kind.empty() && ++tasks_sent_ == self_test.task;
        OSTask test_task;
        if (self_test_now && self_test.kind == "ucode") {
            test_task = *task;
            test_task.t.ucode = static_cast<int32_t>(kTestZeroes);
            std::memset(app_->core.RDRAM + (kTestZeroes - 0x80000000u), 0, 0x2000);
            std::printf("[conker] render self-test (ucode): a task with unknown microcode; the game must stop now\n");
            std::fflush(stdout);
            task = &test_task;
        }
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        require_conker_microcode(*app_, task);
        // The effects the game makes on the CPU over a frame at its swap are made by RT64 after the
        // frame's last draw (src/fixes/frame_effects.cpp).
        if (const uint32_t frame = conker::frame_effects::frame_effects(app_->core.RDRAM, task->t.data_ptr, &frame_effects_)) {
            request_frame_effects(frame);
        }
        // Online, the session's status in the corner, drawn after the game's frame from a list of its
        // own (net_play.cpp), so the game's memory never holds it.
        const uint32_t list = conker::net::overlay_display_list(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFF);
        app_->processDisplayLists(app_->core.RDRAM, list, 0, true);
        if (self_test_now && self_test.kind != "ucode") {
            run_render_self_test(*app_, self_test.kind);
        }
        conker::frame_input::gfx_task_done();
    }

    void send_dummy_workload(uint32_t /*fb_address*/) override {}

    void request_frame_effects(uint32_t frame) {
        using conker::frame_effects::Type;
        for (const conker::frame_effects::Effect& effect : frame_effects_) {
            RT64::FramebufferOperation op;
            if (effect.type == Type::Grey) {
                app_->state->requestGreyFromGreen(frame);
                continue;
            }
            if (effect.type == Type::Mosaic) {
                op.type = RT64::FramebufferOperation::Type::FrameMosaic;
                op.frameMosaic = {frame, effect.mosaic.left, effect.mosaic.top, effect.mosaic.right, effect.mosaic.bottom,
                                  effect.mosaic.step};
            } else {
                op.type = RT64::FramebufferOperation::Type::FrameLens;
                op.frameLens = {frame, effect.lens.center_x, effect.lens.center_y, effect.lens.source_x,
                                effect.lens.source_y, effect.lens.radius, effect.lens.strength};
            }
            app_->state->requestFrameEffect(op);
        }
    }

    void update_screen() override {
        apply_display_scale();
        // Catching up to rejoin a session the game runs many frames a retrace: the display lists are
        // still drawn (the game reads the depth back), but the picture is shown twice a second.
        if (conker::net::catching_up()) {
            const auto now = std::chrono::steady_clock::now();
            if (now - last_catch_up_present_ < std::chrono::milliseconds(500)) {
                return;
            }
            last_catch_up_present_ = now;
        }
        // The game culls what lies outside its own 4:3 view; a widened picture needs it to cull to
        // the wider view (src/enhancements/display_options/widescreen_culling.cpp).
        conker::display::set_rendered_aspect(
            !applied_wide_ ? kPictureAspect
            : holding_     ? app_->userConfig.aspectTarget
                           : expanded_aspect());
        app_->updateScreen();
    }

    void shutdown() override {
        if (app_ != nullptr) {
            app_->end();
        }
    }

    uint32_t get_display_framerate() const override {
        return app_->presentQueue->ext.sharedResources->swapChainRate;
    }

    // The scale RT64 picked for the last frame (resolutionMultiplier is only used in Manual mode).
    float get_resolution_scale() const override {
        float scale;
        uint32_t width, height;
        {
            RT64::SharedQueueResources* shared = app_->sharedQueueResources.get();
            std::scoped_lock lock(shared->configurationMutex);
            scale = shared->resolutionScale.y;
            width = shared->swapChainWidth;
            height = shared->swapChainHeight;
        }
        if (scale != reported_scale_) {
            // A new scale resizes the framebuffers, which keep their old contents; discard them, or the
            // two columns at each side of Conker's frame buffer that the game never draws show a stale
            // picture.
            if (reported_scale_ != 0.0f) {
                app_->updateUserConfig(true);
                char event[64];
                std::snprintf(event, sizeof(event), "RT64 scale %gx: frame buffers discarded", scale);
                conker::gpu_faults::note(event);
            }
        }
        // The window's size reaches RT64 a frame after a new scale, so report either change.
        if (scale != reported_scale_ || width != reported_width_ || height != reported_height_) {
            reported_scale_ = scale;
            reported_width_ = width;
            reported_height_ = height;
            std::printf("[conker] RT64 renders at %gx for a %ux%u window\n", scale, width, height);
            char event[64];
            std::snprintf(event, sizeof(event), "RT64 renders at %gx for a %ux%u window", scale, width, height);
            conker::gpu_faults::note(event);
        }
        return scale;
    }

private:
    static constexpr float kPictureAspect = float(conker::display::kPictureWidth) / conker::display::kPictureHeight;

    // The picture's shape when Expand widens it to the swap chain's: the swap chain's shape, made up
    // for the columns left out at the sides as Expand does, and never narrower than the picture.
    float expanded_aspect() const {
        float target;
        {
            RT64::SharedQueueResources* shared = app_->sharedQueueResources.get();
            std::scoped_lock lock(shared->configurationMutex);
            target = shared->swapChainHeight > 0
                         ? float(shared->swapChainWidth) / float(shared->swapChainHeight)
                         : 16.0f / 9.0f;
        }
        target *= float(conker::display::kPictureWidth) / (conker::display::kPictureWidth - 2.0f * kUndrawnColumns);
        return target > kPictureAspect ? target : kPictureAspect;
    }

    // RT64 renders at the multiple chosen for the mode, in 4:3 or widened to the window's shape, and
    // either scales it to fill the window or screen or presents it at its own size.
    void apply_display_scale() {
        // While the pause menu draws its copy of the last frame, keep the frame buffers as they are,
        // or the copy is lost (see conker::display::note_frame_copy). Expand follows the swap chain's
        // shape, so a widened picture is pinned to the shape it has for the time being. The hold
        // lasts until the game's frames go by without a copy (send_dl counts them); counting the
        // game's frames rather than time or screen refreshes keeps it through the stall of leaving
        // or entering full screen.
        if (tasks_without_copy_ < kHoldTasks) {
            if (!holding_) {
                holding_ = true;
                conker::gpu_faults::note("pause frame copy: display settings held");
                if (applied_wide_) {
                    app_->userConfig.aspectRatio = RT64::UserConfiguration::AspectRatio::Manual;
                    app_->userConfig.aspectTarget = expanded_aspect();
                    app_->updateUserConfig(false);
                }
            }
            return;
        }
        if (holding_) {
            holding_ = false;
            conker::gpu_faults::note("pause frame copy over: display settings let go");
            applied_scale_ = -1;  // apply whatever is asked for now
        }
        // Nor while the window changes mode: new frame buffers in the middle of leaving or entering
        // a full-screen Space stalled everything (see conker::display::window_changing).
        if (conker::display::window_changing()) {
            return;
        }
        const conker::display::Mode mode = conker::display::current();
        const bool windowed = mode == conker::display::Mode::Windowed;
        const int scale = windowed ? conker::display::window_scale() : conker::display::screen_scale();
        const bool native = !conker::display::fill(mode);
        const bool wide = conker::display::widescreen(mode);
        if (scale == applied_scale_ && native == applied_native_ && wide == applied_wide_) {
            return;
        }
        applied_scale_ = scale;
        applied_native_ = native;
        applied_wide_ = wide;
        conker::display::set_rendered_widescreen(wide);
        app_->userConfig.aspectRatio = wide ? RT64::UserConfiguration::AspectRatio::Expand
                                            : RT64::UserConfiguration::AspectRatio::Original;
        app_->userConfig.resolution = scale > 0 ? RT64::UserConfiguration::Resolution::Manual
                                                : RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app_->userConfig.resolutionMultiplier = scale > 0 ? scale : 2.0;
        app_->enhancementConfig.presentation.nativeSize = native;
        app_->enhancementConfig.rect.extendEdgeRects = true;  // only acts on a widened picture
        // The game never draws the two frame-buffer columns at each side (in 4:3 they are the picture's
        // black edges). Widened, nothing draws there either, so they are left out of the picture
        // (tools/patches/rt64-edge-rects.patch) instead of showing as thin bars of black or stale pixels.
        app_->enhancementConfig.presentation.expandCropSides = kUndrawnColumns;
        // The picture covers the whole 292x216 frame buffer, but the VI shows only its rows 1 to 214. In a
        // split-screen match nothing redraws rows 0 and 215, so they kept whatever was last drawn there, such as
        // the rocket launcher's scope, as a band at the bottom. They show black instead
        // (tools/patches/rt64-hidden-rows.patch).
        app_->enhancementConfig.presentation.hideEdgeRows = 1;
        app_->updateUserConfig(true);  // see get_resolution_scale
        app_->updateEnhancementConfig();
        conker::gpu_faults::note("display applied: scale " + std::to_string(scale) + (wide ? ", widescreen" : ", 4:3") +
                                 (native ? ", native size" : ", filled") + "; frame buffers discarded");
    }

    std::unique_ptr<RT64::Application> app_;
    bool packs_loaded_ = false;      // the packs were found (with the first graphics task)
    Packs packs_;
    bool used_sharp_ = true;         // the built-in packs are in use
    conker::pad_icons::Style used_pad_ = conker::pad_icons::Style::Original;  // the button pictures in use
    bool used_swap_ = true;                                                  // and the camera swap they show
    bool widen_font_rects_ = false;  // the built-in font or a texture pack is in use
    std::vector<conker::frame_effects::Effect> frame_effects_;  // the effects on the task's frame
    mutable float reported_scale_ = 0.0f;
    mutable uint32_t reported_width_ = 0;
    mutable uint32_t reported_height_ = 0;
    int applied_scale_ = 0;
    std::chrono::steady_clock::time_point last_catch_up_present_{};
    bool applied_native_ = false;
    bool applied_wide_ = false;
    static constexpr float kUndrawnColumns = 2.0f;
    bool holding_ = false;  // settings held while the pause menu draws its frame copy
    uint32_t seen_copies_ = 0;
    uint64_t tasks_sent_ = 0;  // graphics tasks so far, for CONKER_RENDER_SELFTEST
    int tasks_without_copy_ = 1 << 20;  // graphics tasks since the last frame copy
    uint32_t reported_skips_ = 0;  // write-backs kept out of game memory (online), once reported
    static constexpr int kHoldTasks = 4;
};

} // namespace

void set_wireframe(bool on) { RT64::SetRenderWireframe(on); }

std::vector<std::string> texture_packs() {
    std::lock_guard lock(g_texture_packs_mutex);
    return g_texture_packs;
}

std::string gpu_name() {
    std::lock_guard lock(g_texture_packs_mutex);
    return g_gpu_name;
}

std::string graphics_api() {
    std::lock_guard lock(g_texture_packs_mutex);
    return g_graphics_api;
}

std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<Rt64Renderer>(rdram, window_handle, developer_mode);
}

} // namespace conker::renderer
