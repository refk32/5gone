#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_constants.hpp"

#include <cmath>

// If HAVE_LIQUID is defined by the build (CMake) we use liquid-dsp's FFT,
// exactly like 5GSniffer. Otherwise we use the scalar fallback below.
#ifdef HAVE_LIQUID
#include <liquid/liquid.h>
#endif

namespace gone::nr {

// Basic time unit and scaling constant (see nr_constants.hpp).
static constexpr double kTc = 1.0 / (480000.0 * 4096.0);
static constexpr double kK  = 64.0;

Ofdm::Ofdm(double sample_rate, uint32_t scs_hz, uint16_t num_prbs)
    : sample_rate_(sample_rate), scs_hz_(scs_hz), num_prbs_(num_prbs)
{
    // FFT size = samples per OFDM symbol worth of time = sample_rate / scs.
    // e.g. 23.04e6 / 30e3 = 768.
    fft_size_ = static_cast<uint32_t>(std::llround(sample_rate / static_cast<double>(scs_hz)));
    num_subcarriers_ = num_prbs * PRB_RE;   // 51 * 12 = 612

    // Cyclic prefix lengths (TS 38.211 5.3.1). For mu=1, 30 kHz:
    //   normal CP symbol   = 144*K/2^mu  time units
    //   long CP (symbol 0) = (144*K + 16*K)/2^mu  (only on symbols 0 and 7*2^mu)
    const uint32_t two_mu = 1u << numerology;
    const double useful_length = 2048.0 * kK / static_cast<double>(two_mu);
    const double normal_cp = 144.0 * kK / static_cast<double>(two_mu);
    const double normal_cp_long = (144.0 * kK + 16.0 * kK) / static_cast<double>(two_mu);

    const uint32_t syms_per_subframe = slots_per_subframe * symbols_per_slot; // 28
    samples_per_cp_.resize(syms_per_subframe);
    samples_per_symbol_.resize(syms_per_subframe);

    for (uint32_t l = 0; l < syms_per_subframe; ++l) {
        double cp = normal_cp;
        // Symbols 0 and 7*2^mu (=14) within a subframe carry the longer CP.
        if (l == 0 || l == 7 * two_mu) cp = normal_cp_long;

        // Convert time units -> samples at our sample rate.
        samples_per_cp_[l] = static_cast<uint32_t>(std::floor(cp * kTc * sample_rate_));
        samples_per_symbol_[l] = static_cast<uint32_t>(std::floor((cp + useful_length) * kTc * sample_rate_));
    }

    // Sample count for one full slot (14 symbols). Because mu=1 the subframe
    // pattern repeats each slot, so summing the first 14 CP/symbol lengths works.
    uint64_t sps = 0;
    for (uint32_t l = 0; l < symbols_per_slot_; ++l) {
        sps += samples_per_symbol_[l];
    }
    samples_per_slot_ = static_cast<uint32_t>(sps);
}

std::vector<std::complex<float>> Ofdm::fft(const std::vector<std::complex<float>>& in) const
{
#ifdef HAVE_LIQUID
    // --- liquid-dsp fast path (used on your Linux box) ---
    std::vector<std::complex<float>> out(in.size(), std::complex<float>(0.0f, 0.0f));
    fftplan q = fft_create_plan(in.size(), const_cast<std::complex<float>*>(in.data()),
                                out.data(), LIQUID_FFT_FORWARD, 0);
    fft_execute(q);
    fft_destroy_plan(q);
    return out;
#else
    // --- Scalar DFT fallback (correct, O(N^2), only for non-liquid builds) ---
    const std::size_t N = in.size();
    std::vector<std::complex<float>> out(N, std::complex<float>(0.0f, 0.0f));
    const float twopi = 8.0f * std::atan(1.0f);
    for (std::size_t k = 0; k < N; ++k) {
        std::complex<float> sum(0.0f, 0.0f);
        for (std::size_t n = 0; n < N; ++n) {
            const float angle = -twopi * static_cast<float>(k * n) / static_cast<float>(N);
            sum += in[n] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        out[k] = sum;
    }
    return out;
#endif
}

std::vector<std::complex<float>> Ofdm::ifft(const std::vector<std::complex<float>>& in) const
{
    const std::size_t N = in.size();
#ifdef HAVE_LIQUID
    // Backward transform (unsignalled +j exponent). liquid-dsp does not
    // normalize, so scale by 1/N to make it the true inverse of fft().
    std::vector<std::complex<float>> out(N, std::complex<float>(0.0f, 0.0f));
    fftplan q = fft_create_plan(N, const_cast<std::complex<float>*>(in.data()),
                                out.data(), LIQUID_FFT_BACKWARD, 0);
    fft_execute(q);
    fft_destroy_plan(q);
    const float inv_n = 1.0f / static_cast<float>(N);
    for (std::size_t i = 0; i < N; ++i) out[i] *= inv_n;
    return out;
#else
    // Scalar inverse DFT fallback (correct, O(N^2), only for non-liquid builds).
    std::vector<std::complex<float>> out(N, std::complex<float>(0.0f, 0.0f));
    const float twopi = 8.0f * std::atan(1.0f);
    const float inv_n = 1.0f / static_cast<float>(N);
    for (std::size_t n = 0; n < N; ++n) {
        std::complex<float> sum(0.0f, 0.0f);
        for (std::size_t k = 0; k < N; ++k) {
            const float angle = twopi * static_cast<float>(k * n) / static_cast<float>(N);
            sum += in[k] * std::complex<float>(std::cos(angle), std::sin(angle));
        }
        out[n] = sum * inv_n;
    }
    return out;
#endif
}

std::vector<Symbol> Ofdm::demodulate(const std::vector<std::complex<float>>& iq)
{
    std::vector<Symbol> out;

    // Bookkeeping: which symbol/slot each demodulated OFDM symbol belongs to.
    uint32_t symbol_in_subframe = 0;  // absolute within 1 ms subframe (0..27)
    uint32_t slot_in_frame = 0;       // slot within the 10 ms frame (0..19)
    uint32_t symbol_in_slot = 0;      // symbol within the slot (0..13)

    std::size_t pos = 0;
    const std::size_t N = iq.size();

    // Process whole symbols until the buffer runs out.
    while (pos + samples_per_symbol_[symbol_in_subframe] <= N) {
        const uint32_t cp  = samples_per_cp_[symbol_in_subframe];
        const uint32_t sps = samples_per_symbol_[symbol_in_subframe];

        // Step 1: skip the CP, copy the `fft_size_` useful time samples.
        std::vector<std::complex<float>> useful(fft_size_);
        for (uint32_t i = 0; i < fft_size_; ++i) {
            useful[i] = iq[pos + cp + i];
        }

        // Step 2: FFT -> frequency bins.
        auto bins = fft(useful);

        // Step 3: keep only the active-BWP subcarriers (bin i -> sample i,
        // guard bins are the upper fft_size - num_subcarriers frequencies).
        Symbol s;
        s.samples.resize(num_subcarriers_);
        for (uint32_t i = 0; i < num_subcarriers_ && i < bins.size(); ++i) {
            s.samples[i] = bins[i];
        }

        // Timestamp the symbol for the decoder.
        s.symbol_index = static_cast<uint8_t>(symbol_in_slot);
        s.slot_index   = static_cast<uint8_t>(slot_in_frame);

        out.push_back(std::move(s));

        // Advance to the next symbol, updating counters.
        pos += sps;

        ++symbol_in_subframe;
        ++symbol_in_slot;
        if (symbol_in_slot == symbols_per_slot_) {
            symbol_in_slot = 0;
            slot_in_frame = (slot_in_frame + 1) % slots_per_frame_;
        }
        symbol_in_subframe %= slots_per_subframe * symbols_per_slot;
    }

    return out;
}

std::vector<std::complex<float>> Ofdm::modulate(const std::vector<Symbol>& symbols) const
{
    std::vector<std::complex<float>> out;
    out.reserve(symbols.size() * samples_per_symbol_[0]);

    // Bookkeeping, identical to demodulate(): which symbol/slot we are on.
    uint32_t symbol_in_subframe = 0;
    uint32_t slot_in_frame = 0;
    uint32_t symbol_in_slot = 0;

    // Frequency-domain layout of Symbol.samples, as written by demodulate():
    //   samples[i] <-> bins[i]   (i in [0, num_subcarriers_), guard bins above)

    for (const Symbol& s : symbols) {
        const uint32_t cp  = samples_per_cp_[symbol_in_subframe];
        const uint32_t sps = samples_per_symbol_[symbol_in_subframe];

        // Place the active subcarriers at their FFT bins (guard/DC zeros above).
        std::vector<std::complex<float>> bins(fft_size_, std::complex<float>(0.0f, 0.0f));
        const uint32_t nsc = (uint32_t)s.samples.size();
        for (uint32_t i = 0; i < nsc && i < fft_size_; ++i) {
            bins[i] = s.samples[i];
        }

        // IFFT -> time-domain symbol.
        const auto time = ifft(bins);

        // Cyclic prefix = copy of the tail of the useful block (as transmitted).
        out.insert(out.end(), time.begin() + (fft_size_ - cp), time.end());
        out.insert(out.end(), time.begin(), time.end());

        // Advance to the next symbol, updating counters like demodulate().
        ++symbol_in_subframe;
        ++symbol_in_slot;
        if (symbol_in_slot == symbols_per_slot_) {
            symbol_in_slot = 0;
            slot_in_frame = (slot_in_frame + 1) % slots_per_frame_;
        }
        symbol_in_subframe %= slots_per_subframe * symbols_per_slot;

        if (sps == 0) break;  // defensive; sps is always > 0 in practice
    }

    return out;
}

} // namespace gone::nr
