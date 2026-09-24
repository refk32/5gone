#pragma once

#include "5gone/nr_coreset.hpp"
#include "5gone/nr_dci.hpp"
#include "5gone/nr_symbol.hpp"

#include <complex>
#include <cstdint>
#include <cstring>
#include <vector>

namespace gone::nr {

// Forward declarations (defined in the other nr_* headers).
class Ofdm;
class Pdcch;
class Pdsch;

/*
 * nr_rar_decoder.hpp
 * ==================
 * The high-level orchestrator that ties the whole DL decode chain together:
 *
 *   raw IQ samples (Single B210, 23.04 MHz)  ->  Ofdm::demodulate
 *        ->  Pdcch::process (DM-RS correlation finds the RAR PDCCH)
 *        ->  DciFormat10::parse (read the scheduling DCI)
 *        ->  Pdsch::demodulate (recover the RAR PDSCH DL-SCH transport block)
 *        ->  parse_mac_rar (read RAPID / TA / Temp C-RNTI / UL grant)
 *        ->  log the observed RAR DCI + MAC RAR contents
 *
 * This is a RECEIVE-only ("DL decode + log") proof of concept: with no
 * srsRAN_4G we still FIND the RAR via correlation and log WHERE it is; with
 * srsRAN_4G (`GONE_HAVE_SRSRAN_OLD`) we also recover the 39 DCI bits and,
 * when the PDSCH decodes, the MAC RAR payload (RAPID / TA / TC-RNTI / UL grant).
 */

// A single observed RAR-scheduling DCI (what we log).
struct RarDciObs {
    bool     decoded_bits = false;   // true if the payload bits were recovered
    uint16_t rnti = 0;               // RA-RNTI that matched (0 = not decoded)
    uint8_t  aggregation_level = 0;
    uint8_t  slot = 0;               // slot within frame
    uint8_t  symbol = 0;             // OFDM symbol within slot
    uint8_t  candidate = 0;
    float    correlation = 0.0f;
    // Sample offset (relative to the decoded `iq` buffer) where the slot that
    // carries this RAR begins. Used by the Step-4 UL gate to map the RAR into
    // the absolute Msg3-slot timeline. 0 when the slot start was not located.
    uint64_t slot_start_sample = 0;
    // Decoded DCI fields (only meaningful when decoded_bits is true):
    uint32_t rb_start = 0;
    uint32_t rb_len = 0;
    uint8_t  mcs = 0;
    uint8_t  harq = 0;
    // MAC RAR fields (only meaningful when rar_parsed is true):
    bool     rar_parsed = false;     // true if PDSCH decoded + MAC RAR parsed
    uint8_t  rapid = 0;              // preamble the UE sent
    uint32_t timing_advance = 0;     // 12-bit TA command
    uint16_t t_c_rnti = 0;           // Temporary C-RNTI
    std::vector<uint8_t> ul_grant;   // 27-bit RAR UL grant (bit values, MSB first)
};

class RarDecoder {
public:
    // sample_rate = e.g. 23.04e6, scs_hz = 30000, pci = cell id, bwp_prbs = 51.
    RarDecoder(double sample_rate, uint32_t scs_hz, uint16_t pci, uint16_t bwp_prbs,
               bool verbose = true);
    ~RarDecoder();

    RarDecoder(const RarDecoder&) = delete;
    RarDecoder& operator=(const RarDecoder&) = delete;

    // Demodulate + correlate `iq`, log any RAR DCI found, and return the list.
    // `iq` must start on a slot boundary; `starting_slot_in_frame` names that
    // slot (0..19) in the real gNB frame (DM-RS scrambling is slot-dependent).
    // `cfo_hz` (>0) removes a residual carrier offset before demodulating.
    std::vector<RarDciObs> decode(const std::vector<std::complex<float>>& iq,
                                  uint32_t starting_slot_in_frame = 0,
                                  double cfo_hz = 0.0);

    // --- CORESET-config probe support (5gone-decode --coreset-sweep) ---
    // The decoder defaults to a full-BWP, duration-1, non-interleaved CORESET
    // anchored at PRB 0. A real gNB's CORESET #0 (from pdcchConfigSIB1) may
    // start at a different PRB / duration / interleaver shift, which makes the
    // default systematically blind. set_coreset() re-arms the decoder for
    // another configuration so the sweep can search the whole grid.
    void set_coreset(const Coreset& coreset);

    // Expose the OFDM stage so a sweep demodulates the window ONCE and then
    // re-scans it under many CORESET configs (slot labels are patched in place).
    // `cfo_hz` (>0) removes a residual carrier offset before demodulating; the
    // sweep callers MUST pass it here — the DM-RS correlation smears to noise
    // on this rig's ~4-5 kHz residual CFO otherwise (the old sweep path
    // only corrected CFO in decode(), never in demodulate(), so --coreset-sweep
    // could never see a real PDCCH on live captures).
    std::vector<Symbol> demodulate(const std::vector<std::complex<float>>& iq,
                                   uint32_t starting_slot_in_frame,
                                   double cfo_hz = 0.0);

    // Correlation-only scan: every candidate above a 0 floor is returned with
    // its DM-RS correlation score (no polar/CRC). Cheap enough to call per
    // (config, slot-offset) in a sweep; restores prior thresholds/decode state.
    std::vector<Dci> scan_pdcch(std::vector<Symbol>& symbols);

private:
    double sample_rate_;
    uint32_t scs_hz_;
    uint16_t pci_;
    uint16_t bwp_prbs_;
    bool verbose_;

    Ofdm*   ofdm_ = nullptr;
    Pdcch*  pdcch_ = nullptr;
    Pdsch*  pdsch_ = nullptr;
};

} // namespace gone::nr
