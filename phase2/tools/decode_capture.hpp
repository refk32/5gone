#pragma once

#include "5gone/cell_sync.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_ofdm.hpp"

#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace gone::tool {

struct DecodeParams {
    std::string path;
    double sample_rate = 23.04e6;
    uint32_t scs_hz    = 30000;
    uint16_t pci       = 1;
    uint16_t bwp_prbs  = 51;
    bool cell_sync     = true;
    size_t start_sample = 0;    // used when cell_sync==false
    size_t num_slots    = 0;    // 0 = entire capture from start point
    bool verbose        = true;
};

struct DecodeResult {
    bool ok = false;            // file parsed successfully
    std::string error;
    bool locked = false;        // CellSync found the SSB
    SsbResult ssb;
    std::vector<nr::RarDciObs> rars;
    size_t total_samples    = 0;
    size_t decoded_samples  = 0;
    size_t decoded_start    = 0;
};

inline DecodeResult decode_capture(const DecodeParams& p)
{
    DecodeResult r{};

    // --- Load interleaved cf32 (two floats per complex sample, 8 bytes each) ---
    {
        std::ifstream f(p.path, std::ios::binary | std::ios::ate);
        if (!f) { r.error = "cannot open " + p.path; return r; }
        const auto sz = f.tellg();
        if (sz % 8 != 0 && sz != 0) {
            r.error = "file size (" + std::to_string(static_cast<long long>(sz))
                      + ") not a multiple of 8";
            return r;
        }
        r.total_samples = static_cast<size_t>(sz) / 8;
        f.seekg(0);
        if (r.total_samples == 0) { r.ok = true; return r; }
        std::vector<float> raw(r.total_samples * 2);
        f.read(reinterpret_cast<char*>(raw.data()), sz);
        SampleBuffer iq(r.total_samples);
        for (size_t i = 0; i < r.total_samples; ++i)
            iq[i] = Sample(raw[i * 2], raw[i * 2 + 1]);
        r.ok = true;

        // --- CellSync (optional) ---
        size_t slice_start = p.start_sample;
        size_t slice_len   = r.total_samples - slice_start;

        if (p.cell_sync) {
            AttackConfig cfg;   // defaults: pci=1, scs=30, srate=23.04MHz
            cfg.sample_rate = p.sample_rate;
            cfg.scs_khz     = static_cast<uint8_t>(p.scs_hz / 1000u);
            cfg.pci         = p.pci;
            CellSync sync(cfg);
            SsbResult ssb;
            if (sync.find_ssb(iq, ssb)) {
                r.locked = true;
                r.ssb    = ssb;
                slice_start = ssb.slot_start;
                slice_len   = r.total_samples - slice_start;
            }
        }

        if (p.num_slots > 0) {
            const nr::Ofdm ofdm(p.sample_rate, static_cast<double>(p.scs_hz), p.bwp_prbs);
            const size_t slot_samples = ofdm.samples_per_slot() * p.num_slots;
            if (slot_samples < slice_len) slice_len = slot_samples;
        }
        if (slice_start + slice_len > r.total_samples)
            slice_len = r.total_samples - slice_start;
        r.decoded_samples = slice_len;
        r.decoded_start   = slice_start;

        SampleBuffer slice(iq.begin() + static_cast<std::ptrdiff_t>(slice_start),
                           iq.begin() + static_cast<std::ptrdiff_t>(slice_start + slice_len));
        nr::RarDecoder dec(p.sample_rate, p.scs_hz, p.pci, p.bwp_prbs, p.verbose);
        r.rars = dec.decode(slice);
    }
    return r;
}

} // namespace gone::tool