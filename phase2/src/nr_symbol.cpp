#include "5gone/nr_symbol.hpp"

#include <cmath>

namespace gone::nr {

void Symbol::channel_estimate(const std::vector<std::complex<float>>& dmrs_reference,
                              const std::vector<uint64_t>&            dmrs_indices,
                              uint64_t                                subcarrier_start,
                              uint64_t                                subcarrier_end)
{
    const std::size_t N = samples.size();

    // Buffer sizes match the symbol. We seed the channel filter with 1.0 (= no
    // twist) so equalization is harmless where we have no estimate yet.
    channel_filter.assign(N, std::complex<float>(1.0f, 0.0f));
    noise.assign(N, std::complex<float>(0.0f, 0.0f));
    samples_eq.assign(N, std::complex<float>(0.0f, 0.0f));

    uint64_t prev = subcarrier_start;
    std::size_t ref_index = 0;

    // --- Step 1: estimate channel exactly at each DM-RS position ---
    for (uint64_t dmrs_index : dmrs_indices) {
        if (ref_index >= dmrs_reference.size()) break;
        if (dmrs_index >= N) continue;

        // We transmitted `dmrs_reference` and received `samples[dmrs_index]`.
        // channel = received / expected  ==  received * conj(expected)
        // (because |expected| ~ 1 for QPSK, division = multiply by conj).
        channel_filter[dmrs_index] = samples[dmrs_index] * std::conj(dmrs_reference[ref_index]);

        // --- Step 2: interpolate between the previous estimate and this one ---
        // Linear ramp across the subcarriers in between.
        float distance = static_cast<float>(dmrs_index - prev);
        if (distance > 1.0f && prev < N) {
            std::complex<float> step = (channel_filter[dmrs_index] - channel_filter[prev]) / distance;
            for (uint64_t j = prev + 1; j < dmrs_index && j < N; ++j) {
                channel_filter[j] = channel_filter[j - 1] + step;
            }
        }
        prev = dmrs_index;
        ++ref_index;
    }

    // --- Step 3: normalize + equalize over the candidate's subcarriers ---
    // Average (complex) channel across the whole symbol.
    std::complex<float> total(0.0f, 0.0f);
    for (const auto& cf : channel_filter) total += cf;
    const std::complex<float> avg = total / static_cast<float>(channel_filter.size());
    const float avg_mag_sq = (std::abs(avg) * std::abs(avg)) > 0.0f
                             ? (std::abs(avg) * std::abs(avg)) : 1.0f;

    // Only equalize the subcarrier range we actually care about.
    const std::size_t end = (subcarrier_end < N) ? static_cast<std::size_t>(subcarrier_end + 1) : N;
    for (std::size_t i = 0; i < end; ++i) {
        // Divide received by channel (zero-forcing equalizer):
        //   eq = received * conj(channel) / |channel|^2
        samples_eq[i] = (samples[i] * std::conj(channel_filter[i])) / avg_mag_sq;
        noise[i] = channel_filter[i] - avg;   // deviation from the mean channel
    }

    is_equalized = true;
}

} // namespace gone::nr
