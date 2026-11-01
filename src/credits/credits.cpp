// Adds the port's own card to the end of the ending credits. Native helper called from the
// [[patches.hook]] snippet for func_151E6964 in config/us.toml.
#include <cstdint>
#include <string>
#include <vector>

#include "recomp.h"

namespace {

constexpr int32_t kCreditsTextSet = 3;  // set 3 of the language's entry in bank 0x1C
constexpr int kLoadedSize = 0x30;       // func_151E6964's sp+0x30: the inflated set's length
constexpr int kTextSetArgument = 0x40;  // func_151E6964's a0, kept at sp+0x40: set + 1

constexpr const char* kCardEnd = "*";
constexpr const char* kRollEnd = "=";

// The card's heading, then its names. Packing the US roll frees 51 bytes, and the card takes each
// string's length plus one and two for its "*": more names than fit leave the roll as it was.
constexpr const char* kPortCard[] = {"PORT THANKS TO", "DevOldSchool"};

}  // namespace

// func_151E6964 inflates one text set of bank 0x1C and then makes a table of its strings. A set is
// a list of strings, each ended by one or more NULs (the table skips the extra ones, which Rare's
// tool left to align every string to two bytes). In the credits a "*" string ends a card and "="
// ends the roll; a card's first string is its heading and the rest are names.
//
// The set's buffer is allocated at its exact length, so the card goes in by packing every string
// behind a single NUL, which frees the room, and putting the spare NULs in front of the "=". The
// card comes after Rare's last one, the roll ends at the same place, and the table, built after
// this, finds the same strings plus the card's.
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
    // Not the layout this expects (the roll must end with a card and then "="): leave it as Rare
    // made it.
    if (!current.empty() || strings.size() < 2 || strings.back() != kRollEnd ||
        strings[strings.size() - 2] != kCardEnd) {
        return;
    }

    strings.pop_back();
    for (const char* line : kPortCard) {
        strings.push_back(line);
    }
    strings.push_back(kCardEnd);

    std::string packed;
    for (const std::string& line : strings) {
        packed += line;
        packed += '\0';
    }
    const int32_t spare = size - static_cast<int32_t>(packed.size() + 2);
    if (spare < 0) {
        return;
    }
    packed.append(static_cast<size_t>(spare), '\0');
    packed += kRollEnd;
    packed += '\0';

    for (int32_t i = 0; i < size; i++) {
        MEM_B(i, buffer) = static_cast<int8_t>(packed[static_cast<size_t>(i)]);
    }
}
