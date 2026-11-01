// Checks the protections that keep a mod inside the game: addresses stay in the game's memory, and
// save file names stay in the saves folder. Needs no ROM. Exits nonzero if any check fails.

#include "mod_sandbox_test.h"

int main() {
    Checker c;
    test_save_file_name(c);
    test_address_window(c);
    printf("%d of %d checks passed\n", c.checks() - c.failures(), c.checks());
    return c.failures() == 0 ? 0 : 1;
}
