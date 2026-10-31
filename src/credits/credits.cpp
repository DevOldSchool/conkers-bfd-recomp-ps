// Adds the port's own name to the ending credits. Native helper called from the [[patches.hook]]
// snippet for func_151E6964 in config/us.toml.
#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"

namespace {

constexpr int32_t kCreditsTextSet = 3;  // set 3 of the language's entry in bank 0x1C
constexpr int kLoadedSize = 0x30;       // func_151E6964's sp+0x30: the inflated set's length
constexpr int kTextSetArgument = 0x40;  // func_151E6964's a0, kept at sp+0x40: set + 1

constexpr const char* kCard = "SPECIAL THANKS TO";
constexpr const char* kAfter = "H. YAMAUCHI";
constexpr const char* kAdded = "DevOldSchool";

}  // namespace

// func_151E6964 inflates one text set of bank 0x1C and then makes a table of its strings. A set is
// a list of strings, each ended by one or more NULs (the table skips the extra ones, which Rare's
// tool left to align every string to two bytes). In the credits a "*" string ends a card and "="
// ends the roll; a card's first string is its heading and the rest are names.
//
// The set's buffer is allocated at its exact length, so the name goes in by packing every string
// behind a single NUL, which frees more than enough room, and putting the spare NULs in front of
// the last string. The roll ends with the same "=" at the same place, and the table, built after
// this, finds the same strings plus the new one.
extern "C" void conker_credits_text_hook(uint8_t* rdram, recomp_context* ctx) {
    if (static_cast<int32_t>(MEM_W(kTextSetArgument, ctx->r29)) - 1 != kCreditsTextSet) {
        return;
    }
    const gpr buffer = ctx->r2;
    const int32_t size = static_cast<int32_t>(MEM_W(kLoadedSize, ctx->r29));
    if (size <= 0 || size > 0x10000) {
        return;
    }

    std::vector<std::string> strings;
    std::string current;
    for (int32_t i = 0; i < size; i++) {
        const char c = static_cast<char>(MEM_B(i, buffer));
        if (c != '\0') {
            current += c;
        } else if (!current.empty()) {
            strings.push_back(current);
            current.clear();
        }
    }
    if (!current.empty() || strings.empty()) {
        return;  // not the layout this expects: leave the roll as Rare made it
    }

    bool added = false;
    for (size_t i = 1; i < strings.size(); i++) {
        if (strings[i - 1] == kCard && strings[i] == kAfter) {
            strings.insert(strings.begin() + static_cast<std::ptrdiff_t>(i) + 1, kAdded);
            added = true;
            break;
        }
    }
    if (!added) {
        return;
    }

    std::string packed;
    for (size_t i = 0; i + 1 < strings.size(); i++) {
        packed += strings[i];
        packed += '\0';
    }
    const std::string& last = strings.back();
    const int32_t spare = size - static_cast<int32_t>(packed.size() + last.size() + 1);
    if (spare < 0) {
        return;
    }
    packed.append(static_cast<size_t>(spare), '\0');
    packed += last;
    packed += '\0';

    for (int32_t i = 0; i < size; i++) {
        MEM_B(i, buffer) = static_cast<int8_t>(packed[static_cast<size_t>(i)]);
    }
}
