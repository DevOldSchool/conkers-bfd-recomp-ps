// C library functions the title references that the SDK's C library does not have, or names but the
// console resolves to 0, linked in by tools/ps5/link-title.sh. Each is local to the title (the link's
// version script), so none becomes an export.
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>

// MB_CUR_MAX. The C locale is the only one the title uses, and its characters are one byte.
int ___mb_cur_max(void) {
    return 1;
}

// Advice the kernel may ignore, so ignoring it is a success.
int posix_madvise(void* address, size_t length, int advice) {
    (void)address;
    (void)length;
    (void)advice;
    return 0;
}

// Days from 1970-01-01 to year-month-day in the proleptic Gregorian calendar (month 1 to 12).
static int64_t days_from_civil(int64_t year, int64_t month, int64_t day) {
    year -= (month <= 2) ? 1 : 0;
    const int64_t era = (year >= 0 ? year : year - 399) / 400;
    const int64_t year_of_era = year - era * 400;
    const int64_t day_of_year = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int64_t day_of_era = year_of_era * 365 + year_of_era / 4 - year_of_era / 100 + day_of_year;
    return era * 146097 + day_of_era - 719468;
}

// mktime in UTC: the fields may be out of their ranges, as mktime's may, and are normalised.
time_t timegm(struct tm* tm) {
    int64_t year = (int64_t)tm->tm_year + 1900;
    int64_t month = tm->tm_mon;  // 0 to 11 once carried into the year
    year += month / 12;
    month %= 12;
    if (month < 0) {
        month += 12;
        year--;
    }
    const int64_t days = days_from_civil(year, month + 1, 1) + (int64_t)tm->tm_mday - 1;
    const int64_t seconds = days * 86400 + (int64_t)tm->tm_hour * 3600 + (int64_t)tm->tm_min * 60 + tm->tm_sec;
    const time_t result = (time_t)seconds;
    gmtime_r(&result, tm);
    return result;
}

// The console resolves pathconf to 0. libc++'s std::filesystem::current_path asks it for _PC_PATH_MAX
// to size its buffer; the limits are the same for every path here, so the path is not looked at.
long pathconf(const char* path, int name) {
    (void)path;
    switch (name) {
        case _PC_NAME_MAX:
            return NAME_MAX;
        case _PC_PATH_MAX:
            return PATH_MAX;
        default:
            errno = EINVAL;
            return -1;
    }
}
