// No input device: the null renderer has no window to take keyboard focus.
#include "input.hpp"

namespace conker::input {

void read(uint16_t* buttons, float* x, float* y) {
    *buttons = 0;
    *x = 0.0f;
    *y = 0.0f;
}

} // namespace conker::input
