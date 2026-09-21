#include "5gone/nr_rar_decoder.hpp"

#include "5gone/nr_constants.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_coreset.hpp"
#include "5gone/nr_pdcch.hpp"
#include "5gone/nr_dci.hpp"
#include "5gone/nr_symbol.hpp"
#include "5gone/nr_pdsch.hpp"
#include "5gone/mac_rar.hpp"

#include <cstdio>

namespace gone::nr {

RarDecoder::RarDecoder(double sample_rate, uint32_t scs_hz, uint16_t pci, uint16_t bwp_prbs,
                       bool verbose)
    : sample_rate_(sample_rate), scs_hz_(scs_hz), pci_(pci), bwp_prbs_(bwp_prbs), verbose_(verbose)
{
    // --- Step 5: OFDM demodulator sized to 23.04 MHz / 30 kHz / 51 PRB ---
    ofdm_ = new Ofdm(sample_rate_, scs_hz_, bwp_prbs_);

    // --- Step 6/7: CORESET + PDCCH blind decoder over the full BWP, CSS ---
    Coreset cs;
    cs.frequency_domain_resources = bwp_prbs_;
    cs.duration = 1;                          // 1-symbol CORESET
    cs.cell_id = pci_;                        // DM-RS scrambling id = PCI
    cs.starting_ofdm_symbol_within_slot = 0;
    cs.num_symbols_per_slot = symbols_per_slot;   // 14
    cs.num_slots_per_frame = slots_per_frame;     // 20
    cs.candidates_search_space = {1, 2, 4, 8, 16};

    pdcch_ = new Pdcch();
    pdcch_->set_coreset_info(cs);
    pdcch_->scrambling_id_start = pci_;
    pdcch_->scrambling_id_end   = pci_;        // only watch our cell
    pdcch_->rnti_start = ra_rnti_min;          // 1
    pdcch_->rnti_end   = ra_rnti_max;          // 512 (lab cell: 267 = 0x10b)
    pdcch_->dci_sizes_list = { (uint8_t)dci_format10_bits(bwp_prbs_) };  // 39 @ 51 RB

    // With srsRAN_4G present we also recover the DCI bits; without it we rely on
    // DM-RS correlation only (still finds + logs where the RAR is).
#ifdef GONE_HAVE_SRSRAN_OLD
    pdcch_->decode_enabled = true;
#else
    pdcch_->decode_enabled = false;
#endif

    if (verbose_) {
        std::printf("[rar-dl] RarDecoder: %.2f MHz, %u kHz, PCI %u, BWP %u PRB, DCI1_0=%u bits, decode=%s\n",
                    sample_rate_ / 1e6, scs_hz_ / 1000u, (unsigned)pci_, (unsigned)bwp_prbs_,
                    (unsigned)dci_format10_bits(bwp_prbs_), pdcch_->decode_enabled ? "on (srsRAN-4G)" : "correlation-only");
    }

    pdcch_->initialize_dmrs_seq();   // precompute reference sequences (Step 7)

    // --- PDSCH decoder (recover the RAR's DL-SCH transport block from the grid) ---
    pdsch_ = new Pdsch(sample_rate_, scs_hz_, pci_, bwp_prbs_, verbose_);
}

RarDecoder::~RarDecoder()
{
    delete ofdm_;
    delete pdcch_;
    delete pdsch_;
}

void RarDecoder::set_coreset(const Coreset& coreset)
{
    // Re-arm the PDCCH stage for a different CORESET (used by the config
    // sweep). Scrambling ids / RNTI range / DCI sizes depend on the cell + BWP,
    // not the CORESET, so they are left untouched.
    pdcch_->set_coreset_info(coreset);
    pdcch_->initialize_dmrs_seq();
}

std::vector<Symbol> RarDecoder::demodulate(const std::vector<std::complex<float>>& iq,
                                           uint32_t starting_slot_in_frame)
{
    return ofdm_->demodulate(iq, starting_slot_in_frame);
}

std::vector<Dci> RarDecoder::scan_pdcch(std::vector<Symbol>& symbols)
{
    const std::vector<float> saved_thresh = pdcch_->AL_corr_thresholds;
    const bool saved_decode = pdcch_->decode_enabled;
    pdcch_->AL_corr_thresholds.assign(NUM_ALs, 0.0f);  // report every candidate score
    pdcch_->decode_enabled = false;                    // correlation-only
    auto found = pdcch_->process(symbols, 0);
    pdcch_->AL_corr_thresholds = saved_thresh;
    pdcch_->decode_enabled = saved_decode;
    return found;
}

std::vector<RarDciObs> RarDecoder::decode(const std::vector<std::complex<float>>& iq,
                                          uint32_t starting_slot_in_frame,
                                          double cfo_hz)
{
    // Step 0: optional residual-carrier-offset correction before demodulation.
    //           s(t) = s(t) * exp(-j*2*pi*f_cfo*t)
    std::vector<std::complex<float>> corrected = iq;
    if (cfo_hz != 0.0) {
        const double twopi_f = 2.0 * 8.0 * std::atan(1.0) * cfo_hz / sample_rate_;
        const std::complex<double> step(std::cos(twopi_f), -std::sin(twopi_f));
        std::complex<double> phasor(1.0, 0.0);
        for (auto& s : corrected) {
            const std::complex<float> c(s);
            s = std::complex<float>(
                static_cast<float>(c.real() * phasor.real() - c.imag() * phasor.imag()),
                static_cast<float>(c.real() * phasor.imag() + c.imag() * phasor.real()));
            phasor *= step;
        }
    }

    // Step 1: OFDM demodulate the raw IQ samples into frequency-domain Symbols.
    auto symbols = ofdm_->demodulate(corrected, starting_slot_in_frame);

    // Step 2: run the PDCCH blind decode (DM-RS correlation; polar decode if srsRAN).
    auto found = pdcch_->process(symbols, 0);

    std::vector<RarDciObs> out;
    out.reserve(found.size());

    for (const auto& d : found) {
        RarDciObs obs;
        obs.aggregation_level = d.found_aggregation_level;
        obs.slot    = d.n_slot;
        obs.symbol  = d.n_ofdm;
        obs.candidate = d.found_candidate;
        obs.correlation = d.correlation;
        obs.rnti = d.rnti;

        // Step 3: if we have the decoded payload bits, parse the DCI 1_0 fields.
        if (d.crc_ok && !d.payload.empty()) {
            DciFormat10 dci = DciFormat10::parse(d.payload, bwp_prbs_);
            obs.decoded_bits = dci.valid;
            if (dci.valid) {
                obs.rb_start = dci.n_start_prb;
                obs.rb_len   = dci.n_length_prb;
                obs.mcs      = dci.mcs;
                obs.harq     = dci.harq_process_number;

                // Step 4: recover the RAR PDSCH payload from the slot grid.
                // sym0 = symbol index in `symbols` where symbol 0 of THIS slot sits.
                int sym0 = -1;
                for (size_t i = 0; i < symbols.size(); ++i) {
                    if (symbols[i].slot_index == d.n_slot && symbols[i].symbol_index == 0) {
                        sym0 = (int)i;
                        break;
                    }
                }
                if (sym0 >= 0) {
                    obs.slot_start_sample = symbols[sym0].sample_index;
                    PdschTb tb = pdsch_->demodulate(symbols, (uint32_t)sym0, dci, d.rnti);
                    if (tb.valid && !tb.tb_bytes.empty()) {
                        MacRar rar = parse_mac_rar(tb.tb_bytes);
                        if (rar.valid) {
                            obs.rar_parsed      = true;
                            obs.rapid           = rar.rapid;
                            obs.timing_advance  = rar.timing_advance;
                            obs.t_c_rnti        = rar.t_c_rnti;
                            obs.ul_grant        = rar.ul_grant;
                        }
                    }
                }
            }
        }

        // Log what we observed (this is the "live, DL-decode + log" deliverable).
        if (obs.decoded_bits) {
            std::printf("[rar-dl] RAR DCI: RNTI=%u AL=%u slot=%u sym=%u cand=%u corr=%.3f "
                        "PDSCH RB[%u..%u) len=%u MCS=%u HARQ=%u\n",
                        (unsigned)obs.rnti, (unsigned)obs.aggregation_level,
                        (unsigned)obs.slot, (unsigned)obs.symbol, (unsigned)obs.candidate,
                        obs.correlation, (unsigned)obs.rb_start,
                        (unsigned)(obs.rb_start + obs.rb_len), (unsigned)obs.rb_len,
                        (unsigned)obs.mcs, (unsigned)obs.harq);
            if (obs.rar_parsed) {
                std::printf("[rar-mac] RAPID=%u TA=%u TempC-RNTI=%u UL-grant=",
                            (unsigned)obs.rapid, (unsigned)obs.timing_advance,
                            (unsigned)obs.t_c_rnti);
                for (uint8_t b : obs.ul_grant) std::printf("%02X", (unsigned)b);
                std::printf(" (%zu B)\n", obs.ul_grant.size());
            } else {
                std::printf("[rar-mac] PDSCH decode: No RAR subPDU (DL-SCH CRC fail or non-RAR TB)\n");
            }
        } else if (verbose_) {
            if (pdcch_->decode_enabled) {
                // Decode ran but no RNTI passed CRC: report the range tried so
                // a too-narrow RA-RNTI window is diagnosable (it once silently
                // excluded the cell's real 0x10b for the whole campaign).
                std::printf("[rar-dl] RAR PDCCH correlation-only: AL=%u slot=%u sym=%u "
                            "cand=%u corr=%.3f (DCI CRC failed for RA-RNTI %u..%u)\n",
                            (unsigned)obs.aggregation_level, (unsigned)obs.slot, (unsigned)obs.symbol,
                            (unsigned)obs.candidate, obs.correlation,
                            (unsigned)pdcch_->rnti_start, (unsigned)pdcch_->rnti_end);
            } else {
                std::printf("[rar-dl] RAR PDCCH detected by DM-RS correlation: AL=%u slot=%u sym=%u "
                            "cand=%u corr=%.3f (decode disabled: build with srsRAN-4G for DCI bits)\n",
                            (unsigned)obs.aggregation_level, (unsigned)obs.slot, (unsigned)obs.symbol,
                            (unsigned)obs.candidate, obs.correlation);
            }
        }

        out.push_back(obs);
    }

    return out;
}

} // namespace gone::nr
