#include "5gone/nr_pss.hpp"

#include "5gone/nr_ofdm.hpp"

#include <cmath>
#include <algorithm>


namespace gone::nr {

std::vector<float> nr_pss_sequence(uint16_t n_id2)
{
    // x(i+7) = (x(i+4) + x(i)) mod 2, initial [x(6)..x(0)] = [1 1 1 0 1 1 0].
    std::vector<uint8_t> x(127, 0);
    x[0] = 0; x[1] = 1; x[2] = 1; x[3] = 0; x[4] = 1; x[5] = 1; x[6] = 1;
    for (int i = 7; i < 127; ++i)
        x[i] = (x[i - 3] + x[i - 7]) & 1;   // => x[i] = (x[i-3] + x[i-7]) % 2

    std::vector<float> seq(127);
    const uint32_t n2 = n_id2 % 3u;
    for (int n = 0; n < 127; ++n) {
        const uint32_t m = static_cast<uint32_t>((n + 43 * static_cast<int>(n2)) % 127);
        seq[n] = 1.0f - 2.0f * static_cast<float>(x[m]);
    }
    return seq;
}

std::vector<float> nr_sss_sequence(uint16_t n_id1, uint16_t n_id2)
{
    // x0(i+7) = (x0(i+4) + x0(i)) mod 2,  init [0 0 0 0 0 0 1]  => x0[i] = (x0[i-3] + x0[i-7]) % 2
    // x1(i+7) = (x1(i+1) + x1(i)) mod 2,  init [0 0 0 0 0 0 1]  => x1[i] = (x1[i-7] + x1[i-6]) % 2
    std::vector<uint8_t> x0(127, 0), x1(127, 0);
    x0[6] = 1;
    x1[6] = 1;
    for (int i = 7; i < 127; ++i) {
        x0[i] = (x0[i - 3] + x0[i - 7]) & 1;
        x1[i] = (x1[i - 7] + x1[i - 6]) & 1;
    }

    const uint32_t n1 = n_id1 % 336u;
    const uint32_t n2 = n_id2 % 3u;
    const int m0 = 15 * static_cast<int>(n1 / 112) + 5 * static_cast<int>(n2);
    const int m1 = static_cast<int>(n1 % 112);

    std::vector<float> seq(127);
    for (int n = 0; n < 127; ++n) {
        const float s0 = 1.0f - 2.0f * static_cast<float>(x0[(n + m0) % 127]);
        const float s1 = 1.0f - 2.0f * static_cast<float>(x1[(n + m1) % 127]);
        seq[n] = s0 * s1;
    }
    return seq;
}

void place_pss_in_symbol(std::vector<std::complex<float>>& symbol, uint16_t n_id2)
{
    const auto seq = nr_pss_sequence(n_id2);
    for (int i = 0; i < 127; ++i)
        symbol[kPssFirstSub + i] = std::complex<float>(seq[i], 0.0f);
}

void place_sss_in_symbol(std::vector<std::complex<float>>& symbol,
                         uint16_t n_id1, uint16_t n_id2)
{
    const auto seq = nr_sss_sequence(n_id1, n_id2);
    for (int i = 0; i < 127; ++i)
        symbol[kPssFirstSub + i] = std::complex<float>(seq[i], 0.0f);
}

std::vector<std::complex<float>> pss_time_reference(uint16_t n_id2, uint32_t fft_size,
                                                    double sample_rate, uint32_t scs_hz)
{
    // Build one PSS symbol (k_ssb = 0, SSB at BWP subcarriers 0..239) through
    // our own modulator; the useful body is the last fft_size samples.
    const uint16_t prbs = static_cast<uint16_t>(std::max<uint32_t>(fft_size / 12, 16));
    Ofdm ofdm(sample_rate, scs_hz, prbs);
    std::vector<Symbol> one(1);
    if (ofdm.num_subcarriers() < kPssFirstSub + kPssLen) return {};
    one[0].samples.assign(ofdm.num_subcarriers(), std::complex<float>(0.0f, 0.0f));
    place_pss_in_symbol(one[0].samples, n_id2);

    auto iq = ofdm.modulate(one);
    if (iq.size() < fft_size) return {};
    return std::vector<std::complex<float>>(iq.end() - static_cast<std::ptrdiff_t>(fft_size),
                                            iq.end());
}

static float correlate_fd(const std::vector<std::complex<float>>& symbol,
                          const std::vector<float>& seq,
                          int subcarrier_start, int search_bins, int& best_offset)
{
    const uint32_t len = static_cast<uint32_t>(seq.size());
    double seq_energy = 0.0;
    for (float v : seq) seq_energy += static_cast<double>(v) * v;

    float best = 0.0f;
    best_offset = 0;
    for (int off = -search_bins; off <= search_bins; ++off) {
        const int base = subcarrier_start + off;
        if (base < 0 || base + static_cast<int>(len) > static_cast<int>(symbol.size())) continue;
        std::complex<double> acc(0.0, 0.0);
        double rx_energy = 0.0;
        for (int i = 0; i < static_cast<int>(len); ++i) {
            acc += std::conj(std::complex<double>(seq[i], 0.0)) *
                   std::complex<double>(symbol[base + i]);
            rx_energy += std::norm(std::complex<double>(symbol[base + i]));
        }
        const double corr = std::abs(acc) / (std::sqrt(seq_energy * rx_energy) + 1e-12);
        if (corr > best) {
            best = static_cast<float>(corr);
            best_offset = off;
        }
    }
    return best;
}

float pss_correlate_fd(const std::vector<std::complex<float>>& symbol,
                       uint16_t n_id2, int search_bins, int& best_offset)
{
    return correlate_fd(symbol, nr_pss_sequence(n_id2),
                        static_cast<int>(kPssFirstSub), search_bins, best_offset);
}

float sss_correlate_fd(const std::vector<std::complex<float>>& symbol,
                       uint16_t n_id1, uint16_t n_id2,
                       int search_bins, int& best_offset)
{
    return correlate_fd(symbol, nr_sss_sequence(n_id1, n_id2),
                        static_cast<int>(kPssFirstSub), search_bins, best_offset);
}

std::vector<std::complex<float>> pss_sliding_corr(
    const std::vector<std::complex<float>>& iq,
    const std::vector<std::complex<float>>& ref)
{
    std::vector<std::complex<float>> out;
    const size_t R = ref.size();
    if (R == 0 || iq.size() < R) return out;
    out.resize(iq.size() - R + 1, std::complex<float>(0.0f, 0.0f));
    for (size_t lag = 0; lag + R <= iq.size(); ++lag) {
        std::complex<float> acc(0.0f, 0.0f);
        for (size_t k = 0; k < R; ++k)
            acc += std::conj(ref[k]) * iq[lag + k];
        out[lag] = acc;
    }
    return out;
}

PssAcq acquire_pss(const std::vector<std::complex<float>>& rx,
                   const std::vector<std::complex<float>>& ref,
                   size_t from, size_t len, double sample_rate)
{
    PssAcq a;
    const size_t R = ref.size();
    if (R == 0 || rx.empty()) return a;
    if (from + len > rx.size()) len = rx.size() - from;
    if (len <= R) return a;

    double er = 0.0;
    for (const auto& v : ref) er += std::norm(v);

    size_t best = 0;
    double best_c = -1.0;
    for (size_t k = 0; k + R <= len; ++k) {
        std::complex<double> dot(0.0, 0.0);
        double e = 0.0;
        for (size_t j = 0; j < R; ++j) {
            dot += std::conj(std::complex<double>(ref[j])) *
                   std::complex<double>(rx[from + k + j]);
            e += std::norm(std::complex<double>(rx[from + k + j]));
        }
        const double c = std::abs(dot) / (std::sqrt(er * e) + 1e-12);
        if (c > best_c) { best_c = c; best = k; }
    }

    const size_t off = from + best;
    a.offset = off;
    a.corr = best_c;
    a.found = a.corr >= 0.35;

    // CFO from the two halves of the matched PSS body: the phase advance
    // across R/2 samples is 2*pi*f*T_span.
    const size_t H = R / 2;
    std::complex<double> d1(0.0, 0.0), d2(0.0, 0.0);
    for (size_t j = 0; j < H; ++j) {
        d1 += std::conj(std::complex<double>(ref[j])) *
              std::complex<double>(rx[off + j]);
        d2 += std::conj(std::complex<double>(ref[H + j])) *
              std::complex<double>(rx[off + H + j]);
    }
    if (std::abs(d1) > 1e-12 && std::abs(d2) > 1e-12) {
        const double ph = std::arg(d2 * std::conj(d1));
        const double span_sec = static_cast<double>(H) / sample_rate;
        a.cfo_hz = ph / (2.0 * 3.14159265358979323846 * span_sec);
        // The PSS itself is phase-flippable (attacker copy is -PSS): its phases
        // cancel in the product d2*conj(d1), so `ph` is the pure CFO ramp.
    }
    return a;
}

static double pss_two_half_cfo(const std::vector<std::complex<float>>& rx,
                               const std::vector<std::complex<float>>& ref,
                               size_t off, double sample_rate)
{
    const size_t R = ref.size();
    if (R < 4) return 0.0;
    const size_t H = R / 2;
    std::complex<double> d1(0.0, 0.0), d2(0.0, 0.0);
    for (size_t j = 0; j < H; ++j) {
        d1 += std::conj(std::complex<double>(ref[j])) *
              std::complex<double>(rx[off + j]);
        d2 += std::conj(std::complex<double>(ref[H + j])) *
              std::complex<double>(rx[off + H + j]);
    }
    if (std::abs(d1) < 1e-12 || std::abs(d2) < 1e-12) return 0.0;
    const double ph = std::arg(d2 * std::conj(d1));
    const double span_sec = static_cast<double>(H) / sample_rate;
    return ph / (2.0 * 3.14159265358979323846 * span_sec);
}

std::vector<PssPeak> pss_scan(const std::vector<std::complex<float>>& rx,
                              const std::vector<std::complex<float>>& ref,
                              float gate, double sample_rate)
{
    std::vector<PssPeak> out;
    const size_t R = ref.size();
    if (R == 0 || rx.size() < R) return out;

    double er = 0.0;
    for (const auto& v : ref) er += std::norm(v);
    if (er <= 1e-12) return out;

    const size_t n_corr = rx.size() - R + 1;
    std::vector<float> corr(n_corr);
    for (size_t k = 0; k < n_corr; ++k) {
        std::complex<double> dot(0.0, 0.0);
        double e = 0.0;
        for (size_t j = 0; j < R; ++j) {
            dot += std::conj(std::complex<double>(ref[j])) *
                   std::complex<double>(rx[k + j]);
            e += std::norm(std::complex<double>(rx[k + j]));
        }
        corr[k] = static_cast<float>(std::abs(dot) / (std::sqrt(er * e) + 1e-12));
    }

    for (size_t k = 0; k < n_corr; ++k) {
        if (corr[k] < gate) continue;
        const bool local = (k == 0 || corr[k] >= corr[k - 1]) &&
                           (k + 1 == n_corr || corr[k] >= corr[k + 1]);
        if (!local) continue;
        out.push_back(PssPeak{k, corr[k],
                              pss_two_half_cfo(rx, ref, k, sample_rate)});
    }
    std::sort(out.begin(), out.end(),
              [](const PssPeak& a, const PssPeak& b) { return a.corr > b.corr; });
    constexpr size_t kMaxPeaks = 6;
    if (out.size() > kMaxPeaks) out.resize(kMaxPeaks);
    return out;
}

} // namespace gone::nr