#include "renderer_null.hpp"

#include <cstdio>

namespace conker::renderer {

namespace {

class NullRenderer final : public ultramodern::renderer::RendererContext {
public:
    NullRenderer() {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
    }

    bool valid() override { return true; }

    bool update_config(const ultramodern::renderer::GraphicsConfig& /*old_config*/,
                       const ultramodern::renderer::GraphicsConfig& /*new_config*/) override {
        return true;
    }

    void enable_instant_present() override {}

    void send_dl(const OSTask* task) override {
        frames_++;
        if (frames_ <= 3 || frames_ % 600 == 0) {
            std::printf("[conker] gfx task %llu: ucode 0x%08X (%u bytes) data 0x%08X (%u bytes) dl 0x%08X\n",
                        static_cast<unsigned long long>(frames_), task->t.ucode, task->t.ucode_size,
                        task->t.ucode_data, task->t.ucode_data_size, task->t.data_ptr);
        }
    }

    void send_dummy_workload(uint32_t /*fb_address*/) override {}
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

private:
    unsigned long long frames_ = 0;
};

} // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(
    uint8_t* /*rdram*/, ultramodern::renderer::WindowHandle /*window_handle*/, bool /*developer_mode*/) {
    return std::make_unique<NullRenderer>();
}

} // namespace conker::renderer
