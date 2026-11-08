// Escapes out of Rare's handwritten geometry code: see src/fixes/geometry_escape.h. Native helpers
// called from the [[patches.hook]] snippets for func_150A3A70 in config/us.toml.
#include <cstdint>

#include "recomp.h"
#include "fixes/geometry_escape.h"

namespace {

inline gpr kseg0(uint32_t address) { return static_cast<gpr>(static_cast<int32_t>(address)); }

constexpr uint32_t kGeometryEscapeSp = 0x800D3858;  // D_800D3858: func_150ADAF0's $sp, 0 when idle

struct GeometryEscape {
    jmp_buf buffer;
    uint32_t sp = 0;  // the frame the buffer returns to; 0 when not armed
};

thread_local GeometryEscape t_geometry_escape;

}  // namespace

extern "C" jmp_buf* conker_geometry_escape_arm(recomp_context* ctx) {
    t_geometry_escape.sp = static_cast<uint32_t>(ctx->r29);
    return &t_geometry_escape.buffer;
}

extern "C" void conker_geometry_escape_disarm(uint8_t* /*rdram*/, recomp_context* ctx) {
    if (t_geometry_escape.sp == static_cast<uint32_t>(ctx->r29)) {
        t_geometry_escape.sp = 0;
    }
}

extern "C" void conker_geometry_escape(uint8_t* rdram, recomp_context* ctx) {
    const uint32_t sp = static_cast<uint32_t>(MEM_W(0, kseg0(kGeometryEscapeSp)));
    if (sp == 0 || sp != t_geometry_escape.sp) {
        return;
    }
    // lui $t2; lw $t2, D_800D3858; sw $zero, D_800D3858; b 0x150AE0AC; or $sp, $t2, $zero
    ctx->r10 = kseg0(sp);
    MEM_W(0, kseg0(kGeometryEscapeSp)) = 0;
    ctx->r29 = kseg0(sp);
#if defined(_WIN32)
    longjmp(t_geometry_escape.buffer, 1);
#elif CONKER_PS5
    siglongjmp(reinterpret_cast<struct _sigjmp_buf*>(t_geometry_escape.buffer), 1);
#else
    _longjmp(t_geometry_escape.buffer, 1);
#endif
}
