// The ROM dialog for the Linux and Windows builds started from the desktop (see launch.hpp):
// nativefiledialog-extended, which RT64 already builds (GTK on Linux, the common item dialog on
// Windows).
#include "platform/launch.hpp"

#include <cstdio>
#include <string>

#include "nfd.h"

namespace conker::launch {

std::filesystem::path choose_rom() {
    if (NFD_Init() != NFD_OKAY) {
        std::fprintf(stderr, "[conker] no file dialog: %s\n", NFD_GetError());
        return {};
    }
    const nfdu8filteritem_t filter{"Nintendo 64 ROM", "z64,n64,v64"};
    nfdu8char_t* path = nullptr;
    std::filesystem::path rom;
    if (NFD_OpenDialogU8(&path, &filter, 1, nullptr) == NFD_OKAY) {
        rom = std::filesystem::path(std::u8string(reinterpret_cast<const char8_t*>(path)));  // UTF-8 on Windows too
        NFD_FreePathU8(path);
    }
    NFD_Quit();
    return rom;
}

}  // namespace conker::launch
