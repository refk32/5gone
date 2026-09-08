#pragma once

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
    std::vector<uint8_t> ul_grant;   // 20-bit RAR UL grant (bit-packed)
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
    std::vector<RarDciObs> decode(const std::vector<std::complex<float>>& iq);

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
