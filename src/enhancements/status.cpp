// What each enhancement is set to, its settings, and the computer: see src/enhancements/status.hpp.
#include "enhancements/status.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#if defined(__APPLE__) || defined(__linux__)
#include <sys/utsname.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#if defined(_WIN32)
#include <windows.h>
#endif

#include "platform/display_mode.hpp"
#include "platform/input.hpp"
#include "platform/render_debug.hpp"

namespace conker::status {

namespace {

constexpr const char* kSharpFont = "built-in sharp font";
constexpr const char* kSharpMenus = "built-in sharp menus";
constexpr const char* kPadButtons[][2] = {
    {"built-in N64 buttons", "n64"}, {"built-in Xbox buttons", "xbox"}, {"built-in PlayStation buttons", "playstation"}};

#if defined(__APPLE__)
std::string sysctl_string(const char* name) {
    size_t size = 0;
    if (sysctlbyname(name, nullptr, &size, nullptr, 0) != 0 || size == 0) {
        return "";
    }
    std::string value(size, '\0');
    if (sysctlbyname(name, value.data(), &size, nullptr, 0) != 0) {
        return "";
    }
    value.resize(std::strlen(value.c_str()));
    return value;
}
#endif

}  // namespace

nlohmann::json enhancement(const char* id, const char* name, bool on, const char* variable, nlohmann::json options) {
    return {{"id", id},
            {"name", name},
            {"on", on},
            {"variable", variable != nullptr ? nlohmann::json(variable) : nlohmann::json(nullptr)},
            {"options", std::move(options)}};
}

nlohmann::json enhancements(uint8_t* rdram) {
    const std::vector<std::string> packs = conker::renderer::texture_packs();
    const bool sharp_font = std::find(packs.begin(), packs.end(), kSharpFont) != packs.end();
    const bool sharp_menus = std::find(packs.begin(), packs.end(), kSharpMenus) != packs.end();
    // The pad's button pictures: which pad's, if any (src/enhancements/pad_icons).
    nlohmann::json pad_buttons = "original";
    for (const auto& [pack, pictures] : kPadButtons) {
        if (std::find(packs.begin(), packs.end(), pack) != packs.end()) pad_buttons = pictures;
    }
    // The OPTIONS menu's SHARP TEXT / ORIGINAL TEXT row switches both.
    const nlohmann::json sharp_options = {{"options_row", conker::display::sharp() ? "sharp" : "original"}};
    return nlohmann::json::array({
        display_options(),
        intro_skip(),
        cutscene_skip(),
        pause_save(),
        save_snapshots(),
        surround_default(rdram),
        mp_characters(),
        max_detail(),
        draw_distance(),
        rumble_banner(),
        haptics(),
        adaptive_triggers(),
        consistent_controls(),
        frame_rate(),
        enhancement("sharp_font", "Sharper text", sharp_font, "CONKER_SHARP_FONT", sharp_options),
        enhancement("sharp_menus", "Sharper menus", sharp_menus, "CONKER_SHARP_MENUS", sharp_options),
        enhancement("pad_buttons", "Your pad's buttons", pad_buttons != "original", "CONKER_PAD_ICONS",
                    {{"pictures", pad_buttons}}),
        debug_menu(),
    });
}

nlohmann::json settings() {
    const conker::display::Mode mode = conker::display::current();
    const bool windowed = mode == conker::display::Mode::Windowed;
    const int scale = windowed ? conker::display::window_scale() : conker::display::screen_scale();
    const int width = windowed ? conker::display::window_picture_width(scale) : conker::display::screen_picture_width(scale);
    nlohmann::json packs = nlohmann::json::array();
    for (const std::string& pack : conker::renderer::texture_packs()) {
        packs.push_back(pack);
    }
    const std::string pad = conker::input::controller_model();  // not its name: often the owner's
    return {
        {"display",
         {{"mode", conker::display::name(mode)},
          {"widescreen", conker::display::widescreen(mode)},
          {"scale", scale},
          {"width", width},
          {"height", conker::display::kPictureHeight * scale},
          {"fill", conker::display::fill(mode)},
          {"sharp", conker::display::sharp()}}},
        {"texture_packs", packs},
        {"controller", {{"model", pad.empty() ? nlohmann::json(nullptr) : nlohmann::json(pad)},
                        {"rumble", conker::input::has_rumble()}}},
    };
}

nlohmann::json system() {
    nlohmann::json os = nlohmann::json::object();
    nlohmann::json cpu = {{"threads", std::thread::hardware_concurrency()}};
    nlohmann::json memory = nullptr;
#if defined(__APPLE__) || defined(__linux__)
    utsname names{};
    if (uname(&names) == 0) {
        os["kernel"] = std::string(names.sysname) + " " + names.release;
        os["architecture"] = names.machine;
    }
#endif
#if defined(__APPLE__)
    os["name"] = "macOS";
    os["version"] = sysctl_string("kern.osproductversion");
    cpu["model"] = sysctl_string("machdep.cpu.brand_string");
    uint64_t bytes = 0;
    size_t size = sizeof(bytes);
    if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) == 0) {
        memory = bytes;
    }
#elif defined(__linux__)
    os["name"] = "Linux";
    std::ifstream release("/etc/os-release");
    for (std::string line; std::getline(release, line);) {
        if (line.rfind("PRETTY_NAME=", 0) == 0) {
            std::string value = line.substr(12);
            value.erase(std::remove(value.begin(), value.end(), '"'), value.end());
            os["version"] = value;
        }
    }
    std::ifstream info("/proc/cpuinfo");
    for (std::string line; std::getline(info, line);) {
        if (line.rfind("model name", 0) == 0 && line.find(':') != std::string::npos) {
            cpu["model"] = line.substr(line.find(':') + 2);
            break;
        }
    }
    memory = static_cast<uint64_t>(sysconf(_SC_PHYS_PAGES)) * static_cast<uint64_t>(sysconf(_SC_PAGESIZE));
#elif defined(_WIN32)
    os["name"] = "Windows";
    // RtlGetVersion gives the real version; GetVersionEx stops at 8.1 without a manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOW*);
    auto rtl_get_version = reinterpret_cast<RtlGetVersionFn>(
        reinterpret_cast<void*>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")));
    OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (rtl_get_version != nullptr && rtl_get_version(&version) == 0) {
        os["version"] = std::to_string(version.dwMajorVersion) + "." + std::to_string(version.dwMinorVersion) + "." +
                        std::to_string(version.dwBuildNumber);
    }
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    os["architecture"] = info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_ARM64 ? "arm64" : "x86_64";
    char brand[128] = {};
    DWORD brand_size = sizeof(brand);
    if (RegGetValueA(HKEY_LOCAL_MACHINE, "HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0", "ProcessorNameString",
                     RRF_RT_REG_SZ, nullptr, brand, &brand_size) == ERROR_SUCCESS) {
        cpu["model"] = std::string(brand);
    }
    MEMORYSTATUSEX status{};
    status.dwLength = sizeof(status);
    if (GlobalMemoryStatusEx(&status)) {
        memory = static_cast<uint64_t>(status.ullTotalPhys);
    }
#endif
    const std::string gpu = conker::renderer::gpu_name();
    const std::string api = conker::renderer::graphics_api();
    return {
        {"os", os},
        {"cpu", cpu},
        {"memory_bytes", memory},
        {"gpu", {{"name", gpu.empty() ? nlohmann::json(nullptr) : nlohmann::json(gpu)},
                 {"api", api.empty() ? nlohmann::json(nullptr) : nlohmann::json(api)}}},
        {"display", {{"width", conker::display::display_width()}, {"height", conker::display::display_height()}}},
    };
}

}  // namespace conker::status
