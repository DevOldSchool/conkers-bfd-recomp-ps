// Sine, cosine, tangent and arctangent that give the same bits on every machine, for native code
// that writes the game's memory (mouse look turning the camera, widescreen culling's planes). Online
// play runs one game on Macs, Linux and Windows PCs together, and each system's libm rounds its
// own way in the last bit, which is enough to split the games apart. These use only +, -, *, / and
// sqrt, which IEEE rounds exactly, in double precision, in a fixed order (the build never fuses a
// multiply and an add: -ffp-contract=off), so every machine gets the same float.
#pragma once

#include <cmath>

namespace conker::exact_math {

namespace detail {

// x reduced to r in [-pi/4, pi/4] with x = r + q * pi/2; q's low two bits returned. Good for the
// angles the game has (a few turns), not for huge ones.
inline int reduce(double x, double* r) {
    constexpr double kTwoOverPi = 0.63661977236758134308;
    constexpr double kPiOver2High = 1.5707963267341256e+00;  // pi/2 in two parts: high has 33 bits
    constexpr double kPiOver2Low = 6.0771005065061922e-11;
    const double q = std::nearbyint(x * kTwoOverPi);
    *r = (x - q * kPiOver2High) - q * kPiOver2Low;
    return static_cast<int>(static_cast<long long>(q) & 3);
}

// Taylor series about 0, for |r| <= pi/4 (the next terms are below double's precision for a float).
inline double sin_series(double r) {
    const double r2 = r * r;
    double sum = 1.0 / 6227020800.0;   // 1/13!
    sum = -1.0 / 39916800.0 + r2 * sum;  // 1/11!
    sum = 1.0 / 362880.0 + r2 * sum;
    sum = -1.0 / 5040.0 + r2 * sum;
    sum = 1.0 / 120.0 + r2 * sum;
    sum = -1.0 / 6.0 + r2 * sum;
    return r + r * r2 * sum;
}

inline double cos_series(double r) {
    const double r2 = r * r;
    double sum = -1.0 / 87178291200.0;   // -1/14!
    sum = 1.0 / 479001600.0 + r2 * sum;  // 1/12!
    sum = -1.0 / 3628800.0 + r2 * sum;
    sum = 1.0 / 40320.0 + r2 * sum;
    sum = -1.0 / 720.0 + r2 * sum;
    sum = 1.0 / 24.0 + r2 * sum;
    sum = -0.5 + r2 * sum;
    return 1.0 + r2 * sum;
}

inline double sin_d(double x) {
    double r;
    switch (reduce(x, &r)) {
        case 0: return sin_series(r);
        case 1: return cos_series(r);
        case 2: return -sin_series(r);
        default: return -cos_series(r);
    }
}

inline double cos_d(double x) {
    double r;
    switch (reduce(x, &r)) {
        case 0: return cos_series(r);
        case 1: return -sin_series(r);
        case 2: return -cos_series(r);
        default: return sin_series(r);
    }
}

inline double atan_d(double x) {
    if (std::isnan(x)) {
        return x;
    }
    constexpr double kPiOver2 = 1.57079632679489661923;
    const bool negative = x < 0.0;
    double a = negative ? -x : x;
    double offset = 0.0;
    if (a > 1.0) {  // atan(a) = pi/2 - atan(1/a)
        a = 1.0 / a;
        offset = kPiOver2;
    }
    // atan(a) = 2 atan(a / (1 + sqrt(1 + a^2))), twice: |a| <= tan(pi/16), about 0.199.
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    a = a / (1.0 + std::sqrt(1.0 + a * a));
    const double a2 = a * a;
    double sum = 0.0;
    for (int n = 21; n >= 3; n -= 2) {
        sum = (((n / 2) % 2 == 0) ? 1.0 : -1.0) / n + a2 * sum;
    }
    double result = 4.0 * (a + a * a2 * sum);
    if (offset != 0.0) {
        result = offset - result;
    }
    return negative ? -result : result;
}

}  // namespace detail

inline float sin(float x) { return static_cast<float>(detail::sin_d(x)); }
inline float cos(float x) { return static_cast<float>(detail::cos_d(x)); }
inline float tan(float x) { return static_cast<float>(detail::sin_d(x) / detail::cos_d(x)); }
inline float atan(float x) { return static_cast<float>(detail::atan_d(x)); }

}  // namespace conker::exact_math
