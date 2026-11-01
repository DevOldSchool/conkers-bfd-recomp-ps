#ifndef MOD_SANDBOX_TEST_H
#define MOD_SANDBOX_TEST_H

#include <cstdint>
#include <cstdio>
#include <string>

// Counts and prints failed checks.
class Checker {
public:
    bool expect(bool ok, const std::string& what) {
        if (!ok) {
            printf("FAILED   %s\n", what.c_str());
            failed++;
        }
        checked++;
        return ok;
    }
    int failures() const { return failed; }
    int checks() const { return checked; }
private:
    int failed = 0;
    int checked = 0;
};

// ultramodern's TO_PTR for a u32, in its own file because ultra64.h and recomp.h both define PTR.
uint32_t* to_ptr_u32(uint8_t* rdram, uint64_t reg);

void test_address_window(Checker& c);
void test_save_file_name(Checker& c);

#endif
