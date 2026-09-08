#include "5gone/nr_dsp.hpp"

#include <cmath>

namespace gone::nr {

/*
 * correlate_magnitude_normalized
 * ------------------------------
 * Computes one complex dot product between vector `a` and vector `b`:
 *
 *     dot = sum_over_i  a[i] * conj(b[i])
 *
 * Then returns |dot| / (||a|| * ||b||), which is a number in [0, 1]:
 *     ~1.0  -> the two vectors are the same up to a complex scale (a match)
 *     ~0.0  -> completely different (no match)
 *
 * Note: vectors `a` and `b` must be the same length. The scalar implementation
 * here needs no special libraries, so it compiles anywhere.
 *
 * (5GSniffer does the same thing with the faster "volk" SIMD library;
 *  this scalar version is the easy-to-read equivalent.)
 */
void correlate_magnitude_normalized(std::vector<float>& output,
                                    const std::vector<std::complex<float>>& a,
                                    const std::vector<std::complex<float>>& b)
{
    if (a.size() < b.size() || b.empty()) {
        output.assign(1, 0.0f);
        return;
    }

    std::complex<float> dot(0.0f, 0.0f);
    float a_sq = 0.0f;   // sum of |a[i]|^2
    float b_sq = 0.0f;   // sum of |b[i]|^2

    const std::size_t n = b.size();   // use b's length (the reference)

    for (std::size_t i = 0; i < n; ++i) {
        dot += a[i] * std::conj(b[i]);
        a_sq += std::norm(a[i]);
        b_sq += std::norm(b[i]);
    }

    const float denom = std::sqrt(a_sq * b_sq);
    output.assign(1, denom > 0.0f ? std::abs(dot) / denom : 0.0f);
}

} // namespace gone::nr
