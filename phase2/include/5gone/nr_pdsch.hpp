#pragma once

#include "5gone/nr_dci.hpp"
#include "5gone/nr_symbol.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

/*
 * nr_pdsch.hpp
 * ============
 * PDSCH (Physical Downlink Shared Channel) demodulation for the RAR.
 *
 * After the DCI 1_0 tells us WHERE the RAR PDSCH is (RB allocation, MCS,
 * TDRA row / symbol start), this module recovers the actual DL-SCH transport
 * block bytes carried on that PDSCH.
 *
 * It drives srsRAN_4G's `srsran_pdsch_nr_decode()` (gated behind
 * GONE_HAVE_SRSRAN_OLD), exactly like the polar decode in nr_pdcch.cpp.
 * For a first functional/round-trip version we use an identity channel
 * estimate (the received PDSCH REs equalized by our own channel estimate and
 * re-placed into a full slot resource grid), mirroring srsRAN's own
 * pdsch_nr_test.c.
 */

// Recovered DL-SCH transport block (the MAC PDU lives in `tb_bytes`).
struct PdschTb {
    bool                 valid = false;  // false if decode failed / crc mismatch
    uint32_t             tbs_bits = 0;   // transport block size in bits (TS 38.214)
    std::vector<uint8_t> tb_bytes;       // the DL-SCH payload (MAC PDU) bytes
};

// A single srsRAN configuration/driver object. Construct it once with the cell
// parameters; it owns the srsRAN pdsch object + softbuffers + chest + grids.
class Pdsch {
public:
    // sample_rate/scs decide the numerology; bwp_prbs = active PRBs (51);
    // pci = physical cell id (scrambling + DM-RS init).
    Pdsch(double sample_rate, uint32_t scs_hz, uint16_t pci, uint32_t bwp_prbs,
          bool verbose = true);
    ~Pdsch();

    Pdsch(const Pdsch&) = delete;
    Pdsch& operator=(const Pdsch&) = delete;

    // LIVE path: build the slot resource grid from demodulated OFDM symbols
    // (full BWP, 14 symbols per slot; `sym0` = index of the first symbol of
    // the target slot in `symbols`), then decode the DCI-scheduled PDSCH into
    // a transport block. `rnti` is the RA-RNTI the DCI was found under (used
    // for PDSCH data scrambling).
    PdschTb demodulate(const std::vector<Symbol>& symbols, uint32_t sym0,
                       const DciFormat10& dci, uint16_t rnti);

    // TEST path: decode a PDSCH from a pre-filled full slot resource grid
    // (`rnti` used for scrambling) — used by the round-trip test which feeds
    // a grid produced by srsran_pdsch_nr_encode().
    PdschTb decode_grid(const std::complex<float>* grid, const DciFormat10& dci,
                        uint16_t rnti);

    uint32_t nof_prb() const { return bwp_prbs_; }

private:
#ifdef GONE_HAVE_SRSRAN_OLD
    // Build the srsran_sch_cfg_nr_t from the DCI and run the decode against
    // the given slot grid. Shared by demodulate()/decode_grid().
    PdschTb run_decode(const std::complex<float>* grid, const DciFormat10& dci,
                       uint16_t rnti);
#endif

    double   sample_rate_;
    uint32_t scs_hz_;
    uint16_t pci_;
    uint32_t bwp_prbs_;
    bool     verbose_;

#ifdef GONE_HAVE_SRSRAN_OLD
    // srsRAN_4G handles kept opaque via void* so this header stays free of
    // srsRAN includes (which only exist on the build host).
    void* pdsch_;       // srsran_pdsch_nr_t*
    void* carrier_;     // srsran_carrier_nr_t*
    void* softbuffer_;  // srsran_softbuffer_rx_t*
    void* chest_;       // srsran_chest_dl_res_t*
    std::complex<float>* grid_ = nullptr;   // full slot grid (nof_prb*12*14)
    bool  srsran_ready_ = false;
#endif
};

} // namespace gone::nr
