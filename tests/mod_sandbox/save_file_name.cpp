// recomp_change_save_file turns a mod's name into a file in the mod's save folder, so names that
// could leave that folder must be refused.

#include "librecomp/save_file_name.hpp"

#include "mod_sandbox_test.h"

void test_save_file_name(Checker& c) {
    const char* allowed[] = { "slot1", "Save_2", "a-b", "x", "0123456789" };
    for (const char* name : allowed) {
        c.expect(recomp::mods::save_file_name_ok(name), std::string("save name allowed: ") + name);
    }
    c.expect(recomp::mods::save_file_name_ok(std::string(64, 'a')), "save name allowed: 64 characters");

    const char* refused[] = {
        "", ".", "..", "../x", "../../x", "a/b", "a\\b", "/abs", "\\abs", "C:x", "C:\\x", "~", "~/x",
        "a.b", "a b", "a\tb", "%2e%2e", "\xc3\xa9",
    };
    for (const char* name : refused) {
        c.expect(!recomp::mods::save_file_name_ok(name), std::string("save name refused: \"") + name + "\"");
    }
    c.expect(!recomp::mods::save_file_name_ok(std::string(65, 'a')), "save name refused: 65 characters");
    c.expect(!recomp::mods::save_file_name_ok(std::string("a\0/b", 4)), "save name refused: embedded NUL");
}
