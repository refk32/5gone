#pragma once

#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pss.hpp"

#include <cmath>
#include <complex>
#include <cstdint>
#include <fstream>
#include <string>
#include <vector>

namespace gone::nr {

struct CaptureInfo {
    bool ok = false;
    size_t samples = 0;
    double sample_rate = 0.0;
    uint32_t scs_hz = 0;
    uint16_t pci = 0;
    double cfo_hz = 0.0;
};

// --- cf32 (two interleaved floats per complex sample, 8 bytes each) ---

// Read the whole file into a SampleBuffer.
inline std::vector<std::complex<float>> read_cf32(const std::string& path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    const auto sz = f.tellg();
    if (sz % 8 != 0) return {};                    // not whole complex samples
    const size_t n = static_cast<size_t>(sz) / 8;
    f.seekg(0);
    std::vector<float> raw(n * 2);
    f.read(reinterpret_cast<char*>(raw.data()), static_cast<std::streamsize>(sz));
    std::vector<std::complex<float>> out(n);
    for (size_t i = 0; i < n; ++i)
        out[i] = std::complex<float>(raw[i * 2], raw[i * 2 + 1]);
    return out;
}

// Write a SampleBuffer in cf32 format.
inline bool write_cf32(const std::string& path,
                       const std::vector<std::complex<float>>& sig)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    for (const auto& v : sig) {
        const float re = v.real(), im = v.imag();
        f.write(reinterpret_cast<const char*>(&re), sizeof(float));
        f.write(reinterpret_cast<const char*>(&im), sizeof(float));
    }
    return static_cast<bool>(f);
}

// --- Synthetic lab capture for offline testing ---

// One clean lab SSB-slot transmission (SSS symbol 2, PSS symbol 4, k_ssb=0).
// Returns iq of a single slot (or empty when slots==0). `strength` scales the
// SSB relative to a small noise floor so correlations behave realistically.
inline std::vector<std::complex<float>> synth_ssb_slot(Ofdm& tx, uint16_t pci,
                                                       float strength = 0.8f)
{
    const uint16_t n_id2 = pci % 3;
    const uint16_t n_id1 = static_cast<uint16_t>(pci / 3);
    std::vector<Symbol> slot(14);
    for (auto& s : slot) s.samples.assign(tx.num_subcarriers(), {0, 0});
    place_sss_in_symbol(slot[kSssSlotSymbol].samples, n_id1, n_id2);
    place_pss_in_symbol(slot[kPssSlotSymbol].samples, n_id2);
    auto body = tx.modulate(slot);
    for (auto& v : body) v *= strength;
    return body;
}

// Deterministic "noise-like" preamble (idle-cell look) so tests are reproducible.
inline std::vector<std::complex<float>> synth_noise(std::size_t n)
{
    std::vector<std::complex<float>> b(n);
    for (std::size_t i = 0; i < n; ++i)
        b[i] = std::complex<float>(0.04f * std::sin(0.13 * i),
                                   0.04f * std::cos(0.071 * i));
    return b;
}

// Deterministic complex white noise (splitmix64 hash), lattice-free, for
// null-calibrating correlation gates. An LCG lattice once produced 0.95+
// spurious correlations through the 120-config grid; splitmix64 does not.
// Amp scales both I/Q; the normalized correlator is amplitude-invariant, so
// amp only sets the in-band level relative to any signal.
inline std::vector<std::complex<float>> synth_noise_hash(std::size_t n,
                                                         float amp = 1.0f,
                                                         uint64_t seed = 0x9E3779B97F4A7C15ull)
{
    std::vector<std::complex<float>> b(n);
    uint64_t st = seed;
    for (std::size_t i = 0; i < n; ++i) {
        uint64_t z = (st += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= (z >> 31);
        const float u = (float)((double)(z >> 11) / (double)(1ull << 53));
        z = (st += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        z ^= (z >> 31);
        const float v = (float)((double)(z >> 11) / (double)(1ull << 53));
        b[i] = std::complex<float>((u - 0.5f) * 2.0f * amp,
                                   (v - 0.5f) * 2.0f * amp);
    }
    return b;
}

// Compose a capture: [noise preamble of `preamble` samples] + `slots` SSB slots.
// When pci==0, returns only the noise preamble (idle-cell simulation).
// The preamble is kept SMALL relative to one slot so tests stay fast while the
// CellSync PSS matched-filter peak (strength ~0.15) still clears the "6x mean"
// gate against the preamble's weak background.
inline std::vector<std::complex<float>>
synth_capture(uint16_t pci, size_t slots, float strength = 0.8f)
{
    Ofdm tx(23.04e6, 30000, 51);
    const size_t preamble = 900;   // proportionally ~8% of one slot (11520)
    std::vector<std::complex<float>> out = synth_noise(preamble);
    if (pci == 0) return out;

    for (size_t k = 0; k < slots; ++k) {
        auto slot = synth_ssb_slot(tx, pci, strength);
        out.insert(out.end(), slot.begin(), slot.end());
    }
    return out;
}

// Best-effort header scan: detect sample rate, scs, pci, cfo from a cf32 file.
// This is a light helper for `5gone-decode --info`; leave 0s when unknown.
inline CaptureInfo scan_cf32(const std::string& path)
{
    CaptureInfo info;
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return info;
    const auto sz = f.tellg();
    if (sz % 8 != 0) return info;
    info.samples = static_cast<size_t>(sz) / 8;
    if (info.samples == 0) { info.ok = true; return info; }
    info.ok = true;
    return info;
}

} // namespace gone::nr