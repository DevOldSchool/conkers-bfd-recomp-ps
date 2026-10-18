// RT64 renderer context for the runtime (CONKER_RENDERER=rt64).
//
// RT64 interprets the game's display lists in HLE; Conker's F3DEX2CBFD microcode is supported
// through tools/patches/rt64-f3dex2cbfd.patch. The context runs on ultramodern's gfx thread, the
// window it draws into is created on the main thread (src/platform/window_sdl.cpp).
//
// RT64 renders at the whole multiple of the game's picture that covers the window height (its
// WindowIntegerScale default), which follows the window when it is resized or made full screen.
// For a window sized by src/platform/window_sdl.cpp that multiple fills the window exactly. When
// the screen resolution chosen in OPTIONS (src/platform/display_mode.hpp) is an exact multiple, a
// screen-covering mode renders at that multiple and presents it at its own size, centred
// (presentation.nativeSize, tools/patches/rt64-native-size.patch).
#include "platform/renderer_rt64.hpp"

#include <cstdio>
#include <memory>
#include <mutex>

#include "hle/rt64_application.h"
#include "platform/display_mode.hpp"

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
        app_->state->rsp->reset();
        app_->interpreter->loadUCodeGBI(task->t.ucode & 0x3FFFFFF, task->t.ucode_data & 0x3FFFFFF, true);
        app_->processDisplayLists(app_->core.RDRAM, task->t.data_ptr & 0x3FFFFFF, 0, true);
    }

    void send_dummy_workload(uint32_t /*fb_address*/) override {}

    void update_screen() override {
        apply_display_scale();
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
            reported_scale_ = scale;
            std::printf("[conker] RT64 renders at %gx for a %ux%u window\n", scale, width, height);
        }
        return scale;
    }

private:
    // An exact multiple only covering the screen; a window is already a multiple of the picture.
    void apply_display_scale() {
        const int scale = conker::display::current() == conker::display::Mode::Windowed ? 0 : conker::display::screen_scale();
        if (scale == applied_scale_) {
            return;
        }
        applied_scale_ = scale;
        app_->userConfig.resolution = scale > 0 ? RT64::UserConfiguration::Resolution::Manual
                                                : RT64::UserConfiguration::Resolution::WindowIntegerScale;
        app_->userConfig.resolutionMultiplier = scale > 0 ? scale : 2.0;
        app_->enhancementConfig.presentation.nativeSize = scale > 0;
        app_->updateUserConfig(false);
        app_->updateEnhancementConfig();
    }

    std::unique_ptr<RT64::Application> app_;
    mutable float reported_scale_ = 0.0f;
    int applied_scale_ = 0;
};

} // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_rt64_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle window_handle, bool developer_mode) {
    return std::make_unique<Rt64Renderer>(rdram, window_handle, developer_mode);
}

} // namespace conker::renderer
