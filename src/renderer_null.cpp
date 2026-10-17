#include "renderer_null.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

namespace conker::renderer {

namespace {

// --- Display-list dumper ----------------------------------------------------------------------
// CONKER_DUMP_DL=<task>[,<task>...] prints every command of those graphics tasks (1-based task
// numbers), walking F3DEX2-style display lists from the task's data pointer: gSPDisplayList (DE)
// is followed (call or branch), gSPEndDisplayList (DF) returns, segment bases set with
// gSPSegment (DB 06) are applied. Image commands (FD/FE/FF) print their resolved address.
// With the variable set, a one-line summary (command count, content hash, colour image, textures)
// is also printed every 60 tasks and whenever the number of walked commands changes.
// CONKER_DUMP_RDRAM=1 additionally writes the 8 MiB RDRAM, big-endian as on the console, to
// conker-task-<n>.rdram in the working directory for each fully dumped task (offline decoding).

constexpr uint32_t kRdramSize = 0x800000;
constexpr int kMaxDepth = 18;
constexpr int kMaxCommands = 200000;

const char* opcode_name(uint8_t op) {
    if (op >= 0x10 && op <= 0x1F) return "TRI4(CBFD)";
    switch (op) {
        case 0x00: return "NOOP";
        case 0x01: return "VTX";
        case 0x02: return "MODIFYVTX";
        case 0x03: return "CULLDL";
        case 0x04: return "BRANCH_Z";
        case 0x05: return "TRI1";
        case 0x06: return "TRI2";
        case 0x07: return "QUAD";
        case 0xD7: return "TEXTURE";
        case 0xD8: return "POPMTX";
        case 0xD9: return "GEOMETRYMODE";
        case 0xDA: return "MTX";
        case 0xDB: return "MOVEWORD";
        case 0xDC: return "MOVEMEM";
        case 0xDD: return "LOAD_UCODE(CBFD: adv. lighting)";
        case 0xDE: return "DL";
        case 0xDF: return "ENDDL";
        case 0xE0: return "SPNOOP";
        case 0xE1: return "RDPHALF_1";
        case 0xE2: return "SETOTHERMODE_L";
        case 0xE3: return "SETOTHERMODE_H";
        case 0xE4: return "TEXRECT";
        case 0xE5: return "TEXRECTFLIP";
        case 0xE6: return "RDPLOADSYNC";
        case 0xE7: return "RDPPIPESYNC";
        case 0xE8: return "RDPTILESYNC";
        case 0xE9: return "RDPFULLSYNC";
        case 0xEA: return "SETKEYGB";
        case 0xEB: return "SETKEYR";
        case 0xEC: return "SETCONVERT";
        case 0xED: return "SETSCISSOR";
        case 0xEE: return "SETPRIMDEPTH";
        case 0xEF: return "RDPSETOTHERMODE";
        case 0xF0: return "LOADTLUT";
        case 0xF1: return "RDPHALF_2";
        case 0xF2: return "SETTILESIZE";
        case 0xF3: return "LOADBLOCK";
        case 0xF4: return "LOADTILE";
        case 0xF5: return "SETTILE";
        case 0xF6: return "FILLRECT";
        case 0xF7: return "SETFILLCOLOR";
        case 0xF8: return "SETFOGCOLOR";
        case 0xF9: return "SETBLENDCOLOR";
        case 0xFA: return "SETPRIMCOLOR";
        case 0xFB: return "SETENVCOLOR";
        case 0xFC: return "SETCOMBINE";
        case 0xFD: return "SETTIMG";
        case 0xFE: return "SETZIMG";
        case 0xFF: return "SETCIMG";
        default: return "?";
    }
}

struct DlSummary {
    unsigned commands = 0;
    unsigned triangles = 0;
    unsigned texrects = 0;
    unsigned fillrects = 0;
    unsigned dl_calls = 0;
    uint64_t hash = 1469598103934665603ull;
    uint32_t color_image = 0;
    std::set<uint32_t> textures;
    std::set<uint32_t> lists;
    bool truncated = false;
};

class DlWalker {
public:
    DlWalker(const uint8_t* rdram, bool print) : rdram_(rdram), print_(print) {}

    DlSummary walk(uint32_t start) {
        std::memset(segments_, 0, sizeof(segments_));
        uint32_t stack[kMaxDepth];
        int depth = 0;
        uint32_t pc = start & 0x00FFFFFF;
        summary_.lists.insert(pc);
        while (summary_.commands < static_cast<unsigned>(kMaxCommands)) {
            if (pc + 8 > kRdramSize) {
                note(pc, "display list pointer outside RDRAM; stopping");
                summary_.truncated = true;
                break;
            }
            const uint32_t w0 = read32(pc), w1 = read32(pc + 4);
            const uint8_t op = static_cast<uint8_t>(w0 >> 24);
            summary_.commands++;
            summary_.hash = (summary_.hash ^ w0) * 1099511628211ull;
            summary_.hash = (summary_.hash ^ w1) * 1099511628211ull;
            if (print_) {
                std::printf("[conker] dl %*s%06X: %08X %08X  %s", depth * 2, "", pc, w0, w1, opcode_name(op));
                describe(op, w0, w1);
                std::printf("\n");
            }
            pc += 8;
            if (op == 0xDB && ((w0 >> 16) & 0xFF) == 0x06) {  // gSPSegment
                segments_[((w0 & 0xFFFF) / 4) & 0xF] = w1 & 0x00FFFFFF;
            } else if (op == 0xDE) {
                summary_.dl_calls++;
                const uint32_t target = resolve(w1);
                summary_.lists.insert(target);
                if (((w0 >> 16) & 0xFF) == 0) {  // call: push the return address
                    if (depth >= kMaxDepth) {
                        note(pc, "display list stack overflow; stopping");
                        summary_.truncated = true;
                        break;
                    }
                    stack[depth++] = pc;
                }
                pc = target;
            } else if (op == 0xDF) {
                if (depth == 0) break;
                pc = stack[--depth];
            } else if (op == 0x05) {
                summary_.triangles += 1;
            } else if (op == 0x06 || op == 0x07) {
                summary_.triangles += 2;
            } else if (op >= 0x10 && op <= 0x1F) {
                summary_.triangles += 4;
            } else if (op == 0xE4 || op == 0xE5) {
                summary_.texrects++;
            } else if (op == 0xF6) {
                summary_.fillrects++;
            } else if (op == 0xFD) {
                summary_.textures.insert(resolve(w1));
            } else if (op == 0xFF) {
                summary_.color_image = resolve(w1);
            }
        }
        if (summary_.commands >= static_cast<unsigned>(kMaxCommands)) {
            summary_.truncated = true;
        }
        return summary_;
    }

private:
    uint32_t read32(uint32_t addr) const {
        uint32_t value;
        std::memcpy(&value, rdram_ + addr, 4);  // rdram holds native-endian 32-bit words
        return value;
    }

    uint32_t resolve(uint32_t addr) const {
        return (segments_[(addr >> 24) & 0xF] + (addr & 0x00FFFFFF)) & 0x00FFFFFF;
    }

    void note(uint32_t pc, const char* what) const {
        if (print_) std::printf("[conker] dl %06X: %s\n", pc, what);
    }

    void describe(uint8_t op, uint32_t w0, uint32_t w1) const {
        static const char* const fmts[] = {"RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"};
        static const char* const sizes[] = {"4b", "8b", "16b", "32b"};
        switch (op) {
            case 0xDE:
                std::printf(" -> %06X%s", resolve(w1), ((w0 >> 16) & 0xFF) ? " (branch)" : "");
                break;
            case 0xDB:
                if (((w0 >> 16) & 0xFF) == 0x06) std::printf(" segment %u = %06X", ((w0 & 0xFFFF) / 4) & 0xF, w1 & 0x00FFFFFF);
                break;
            case 0x01:
            case 0xDA:
            case 0xDC:
                std::printf(" @%06X", resolve(w1));
                break;
            case 0xFD:
            case 0xFF:
                std::printf(" %s %s width %u @%06X", fmts[(w0 >> 21) & 7], sizes[(w0 >> 19) & 3], (w0 & 0xFFF) + 1, resolve(w1));
                break;
            case 0xFE:
                std::printf(" @%06X", resolve(w1));
                break;
            case 0xF6:
            case 0xE4:
                std::printf(" (%u,%u)-(%u,%u)", ((w1 >> 12) & 0xFFF) >> 2, (w1 & 0xFFF) >> 2, ((w0 >> 12) & 0xFFF) >> 2, (w0 & 0xFFF) >> 2);
                break;
            default:
                break;
        }
    }

    const uint8_t* rdram_;
    bool print_;
    uint32_t segments_[16];
    DlSummary summary_;
};

class NullRenderer final : public ultramodern::renderer::RendererContext {
public:
    explicit NullRenderer(uint8_t* rdram) : rdram_(rdram) {
        setup_result = ultramodern::renderer::SetupResult::Success;
        chosen_api = ultramodern::renderer::GraphicsApi::Auto;
        if (const char* spec = std::getenv("CONKER_DUMP_DL")) {
            dump_enabled_ = true;
            for (const char* p = spec; *p != '\0';) {
                char* end;
                const unsigned long long n = std::strtoull(p, &end, 0);
                if (end == p) break;
                dump_tasks_.insert(n);
                p = (*end == ',') ? end + 1 : end;
            }
        }
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
                        frames_, task->t.ucode, task->t.ucode_size,
                        task->t.ucode_data, task->t.ucode_data_size, task->t.data_ptr);
        }
        if (dump_enabled_ && rdram_ != nullptr) {
            const bool full = dump_tasks_.count(frames_) != 0;
            if (full) {
                std::printf("[conker] dl dump of gfx task %llu (dl 0x%08X)\n", frames_, task->t.data_ptr);
                if (std::getenv("CONKER_DUMP_RDRAM") != nullptr) {
                    write_rdram_image();
                }
            }
            DlWalker walker{rdram_, full};
            const DlSummary s = walker.walk(task->t.data_ptr);
            const bool changed = s.commands != last_commands_;
            if (full || frames_ % 60 == 0 || changed) {
                std::string textures;
                for (uint32_t t : s.textures) {
                    char buf[16];
                    std::snprintf(buf, sizeof(buf), " %06X", t);
                    textures += buf;
                }
                std::printf("[conker] dl summary task %llu: dl %06X, %u cmds, %u lists, %u tris, %u texrects, %u fillrects, "
                            "cimg %06X, hash %016llX%s%s, %zu textures:%s\n",
                            frames_, task->t.data_ptr & 0x00FFFFFF, s.commands, static_cast<unsigned>(s.lists.size()),
                            s.triangles, s.texrects, s.fillrects, s.color_image, static_cast<unsigned long long>(s.hash),
                            s.truncated ? " (truncated)" : "", changed ? " [changed]" : "", s.textures.size(), textures.c_str());
            }
            last_commands_ = s.commands;
        }
    }

    void write_rdram_image() const {
        char path[64];
        std::snprintf(path, sizeof(path), "conker-task-%llu.rdram", frames_);
        FILE* f = std::fopen(path, "wb");
        if (f == nullptr) return;
        for (uint32_t addr = 0; addr < kRdramSize; addr += 4) {
            uint32_t word;
            std::memcpy(&word, rdram_ + addr, 4);
            const uint8_t bytes[4] = {static_cast<uint8_t>(word >> 24), static_cast<uint8_t>(word >> 16),
                                      static_cast<uint8_t>(word >> 8), static_cast<uint8_t>(word)};
            std::fwrite(bytes, 1, 4, f);
        }
        std::fclose(f);
        std::printf("[conker] wrote %s\n", path);
    }

    void send_dummy_workload(uint32_t /*fb_address*/) override {}
    void update_screen() override {}
    void shutdown() override {}
    uint32_t get_display_framerate() const override { return 60; }
    float get_resolution_scale() const override { return 1.0f; }

private:
    uint8_t* rdram_;
    unsigned long long frames_ = 0;
    bool dump_enabled_ = false;
    std::set<unsigned long long> dump_tasks_;
    unsigned last_commands_ = 0;
};

} // namespace

std::unique_ptr<ultramodern::renderer::RendererContext> create_null_render_context(
    uint8_t* rdram, ultramodern::renderer::WindowHandle /*window_handle*/, bool /*developer_mode*/) {
    return std::make_unique<NullRenderer>(rdram);
}

} // namespace conker::renderer
