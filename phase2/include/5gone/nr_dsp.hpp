#pragma once

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_dsp.hpp
 * ==========
 * A couple of small math helpers the PDCCH decoder needs. For now just one:
 *
 * correlate_magnitude_normalized()
 *
 * "How similar are two signals?" is answered by CORRELATION. We correlate the
 * received symbol's subcarriers against our locally-generated DM-RS reference.
 * If they match, the output is close to 1.0; if not, it's small.
 *
 * We use the "normalized" version so the result is always in [0, 1], making
 * the threshold easy to reason about.
 */
void correlate_magnitude_normalized(std::vector<float>& output,
                                    const std::vector<std::complex<float>>& a,
                                    const std::vector<std::complex<float>>& b);

} // namespace gone::nr
