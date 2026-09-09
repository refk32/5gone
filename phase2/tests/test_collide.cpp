#include "5gone/nr_capture.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_rar_decoder.hpp"
#include "5gone/nr_rar_tx.hpp"
#include "5gone/types.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <tuple>
#include <vector>

using namespace gone;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    const double srate = 23.04e6;
    const uint32_t scs = 30000;
    const uint16_t pci = 1, prbs = 51;

    // Synthetic lab gNB RAR slot + a louder conflicting attack copy (payload
    // phase-flipped, x10) — the exact objects --mode collide transmits.
    nr::Ofdm tx(srate, scs, prbs);
    const nr::RarSlotTx B = nr::build_rar_slot(tx, pci, prbs, 0.5f);
    SampleBuffer legit = B.legit;
    SampleBuffer attack = B.attack;
    for (auto& v : attack) v *= 10.0f;
    printf("[collide] slot=%zu samples grid=%zu symbols\n",
           B.slot_samples, B.grid.size());
    CHECK(B.legit.size() == B.slot_samples, "legit is exactly one slot");
    CHECK(B.grid.size() == 14, "grid has 14 symbols");

    // Victim scorer, mirrors run_collide()'s score(): scan the whole window for
    // the PSS (full-rate correlation — no decimation, no timing assumption),
    // pick the strongest peak, slice the slot there, derotate its CFO, decode
    // RAR PDCCHs and compute per-symbol message-2 clarity against the grid.
    nr::RarDecoder dec(srate, scs, pci, prbs, false);
    nr::Ofdm victim_tx(srate, scs, prbs);
    const uint32_t fft = static_cast<uint32_t>(std::llround(srate / scs));
    const auto pss_ref = nr::pss_time_reference(pci % 3, fft, srate, scs);
    const size_t sym4 = B.symbol_offsets[4] + victim_tx.cp_len(4);
    auto score = [&](const SampleBuffer& win) {
        auto peaks = nr::pss_scan(win, pss_ref, 0.5f, srate);
        if (peaks.empty() || peaks.front().offset < sym4)
            return std::make_tuple(false, size_t(0), size_t(0),
                                   std::vector<float>(), 0.0, 0.0, size_t(0));
        const size_t slot_start = peaks.front().offset - sym4;
        if (slot_start + B.slot_samples > win.size())
            return std::make_tuple(false, size_t(0), size_t(0),
                                   std::vector<float>(), 0.0, 0.0, size_t(0));
        SampleBuffer aligned(win.begin() + static_cast<ptrdiff_t>(slot_start),
                             win.begin() + static_cast<ptrdiff_t>(slot_start + B.slot_samples));
        const double w = 2.0 * M_PI * peaks.front().cfo_hz / srate;
        for (size_t i = 0; i < aligned.size(); ++i)
            aligned[i] *= std::exp(std::complex<double>(0.0, -w * static_cast<double>(i)));
        auto rar = dec.decode(aligned);
        auto syms = victim_tx.demodulate(aligned);
        std::vector<float> clr;
        for (uint16_t sy = 2; sy <= 13; ++sy) {
            if (sy >= syms.size()) break;
            const auto& rx = syms[sy].samples;
            const auto& rg = B.grid[sy].samples;
            std::complex<double> dot(0.0, 0.0);
            double ea = 0.0, eg = 0.0;
            const size_t L = std::min(rx.size(), rg.size());
            for (size_t k = 0; k < L; ++k) {
                dot += rx[k] * std::conj(rg[k]);
                ea += std::norm(rx[k]);
                eg += std::norm(rg[k]);
            }
            clr.push_back(ea > 1e-12 && eg > 1e-12
                              ? static_cast<float>(dot.real() / std::sqrt(ea * eg))
                              : 0.0f);
        }
        return std::make_tuple(true, slot_start, rar.size(), clr,
                               peaks.front().corr, peaks.front().cfo_hz,
                               peaks.size());
    };
    auto minclr = [](const std::vector<float>& v) {
        return v.empty() ? 0.0f : *std::min_element(v.begin(), v.end());
    };

    // 1) Control: legit RAR slot only -> clean message-2, RAR PDCCH observed.
    const size_t base = 9000;
    SampleBuffer ctl = nr::synth_noise(base + 2 * B.slot_samples);
    for (size_t t = 0; t < legit.size(); ++t) ctl[base + t] += 15.0f * legit[t];
    const auto c = score(ctl);
    const size_t c_rar = std::get<2>(c);
    const float cmin = minclr(std::get<3>(c));
    printf("[collide] control: acquired=%d slot=%zu rar_obs=%zu msg2_min=%.3f "
           "pss=%.3f cfo=%.0fHz peaks=%zu\n", std::get<0>(c), std::get<1>(c), c_rar,
           cmin, std::get<4>(c), std::get<5>(c), std::get<6>(c));
    CHECK(std::get<0>(c), "control PSS acquired");
    CHECK(std::get<1>(c) == base, "control acquired at injected slot start");
    CHECK(c_rar >= 1, "control RAR PDCCH found");
    CHECK(cmin >= 0.5f, "control message-2 clarity intact");

    // 2) Attack: conflicting copy advanced 1 symbol into the slot -> the
    //    overlapped message-2 symbols flip sign (clarity drops well below 0.5)
    //    and/or a second (louder) RAR PDCCH/SSB peak appears.
    const size_t adv = B.symbol_offsets[1];
    SampleBuffer att = nr::synth_noise(base + 2 * B.slot_samples + adv);
    for (size_t t = 0; t < legit.size(); ++t) att[base + t] += 15.0f * legit[t];
    for (size_t t = 0; t < attack.size(); ++t) att[base + adv + t] += 3.0f * attack[t];
    const auto a = score(att);
    const size_t a_rar = std::get<2>(a);
    const float amin = minclr(std::get<3>(a));
    printf("[collide] attack d=1 (adv=%zu): acquired=%d slot=%zu rar_obs=%zu "
           "msg2_min=%.3f pss=%.3f peaks=%zu\n", adv, std::get<0>(a), std::get<1>(a),
           a_rar, amin, std::get<4>(a), std::get<6>(a));
    CHECK(std::get<0>(a), "attack PSS acquired (re-locks to louder copy)");
    CHECK(amin < cmin - 0.5f || amin < -0.3f,
          "attack flips/collapses victim message-2 clarity");
    CHECK(a_rar >= c_rar || std::get<3>(a).size() < std::get<3>(c).size(),
          "attack adds RAR PDCCH / collapses grid");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}