// CONKER_TEST_OFF=<name>[,<name>...]: turns named fixes off, for regression cases that show they can
// see the bug the fix is for (tools/regress.py, "without_fix"). Test runs only: nothing in the game
// sets it, and a fix not named keeps working.
#pragma once

#include <cstdlib>
#include <cstring>

namespace conker {

inline bool test_off(const char* name) {
    static const char* list = std::getenv("CONKER_TEST_OFF");
    if (list == nullptr) {
        return false;
    }
    const size_t length = std::strlen(name);
    for (const char* at = list; *at != '\0';) {
        const char* end = std::strchr(at, ',');
        const size_t size = end != nullptr ? static_cast<size_t>(end - at) : std::strlen(at);
        if (size == length && std::strncmp(at, name, length) == 0) {
            return true;
        }
        if (end == nullptr) {
            break;
        }
        at = end + 1;
    }
    return false;
}

}  // namespace conker
