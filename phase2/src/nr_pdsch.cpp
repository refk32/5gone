#include "5gone/nr_pdsch.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#ifdef GONE_HAVE_SRSRAN_OLD
// Old srsRAN 4G C API for the PDSCH/DL-SCH decode path. Included at file scope
// (srsran.h opens with `extern "C" {`). All PDSCH symbols here are C linkage.
#include <srsran/srsran.h>
#include <srsran/phy/fec/softbuffer.h>
#include <srsran/phy/ch_estimation/chest_dl.h>
#include <srsran/phy/phch/pdsch_nr.h>
#include <srsran/phy/phch/ra_nr.h>
#endif

namespace gone::nr {

// ---------------------------------------------------------------- ctor/dtor

Pdsch::Pdsch(double sample_rate, uint32_t scs_hz, uint16_t pci, uint32_t bwp_prbs,
             bool verbose)
    : sample_rate_(sample_rate), scs_hz_(scs_hz), pci_(pci), bwp_prbs_(bwp_prbs),
      verbose_(verbose)
{
#ifdef GONE_HAVE_SRSRAN_OLD
    pdsch_      = nullptr;
    carrier_    = nullptr;
    softbuffer_ = nullptr;
    chest_      = nullptr;
    grid_       = nullptr;
    srsran_ready_ = false;

    // ---- Build the srsRAN_4G carrier description ----
    // 30 kHz SCS -> enum value 1 (srsran_subcarrier_spacing_30kHz).
    // Our whole BWP is the carrier: bwp_prbs active PRBs, single layer port 0.
    uint32_t scs_enum = (scs_hz_ <= 15000) ? 0U : 1U;
    srsran_carrier_nr_t carrier = {};
    carrier.pci                   = pci_;
    carrier.dl_center_frequency_hz = 3.5e9;
    carrier.ul_center_frequency_hz = 3.5e9;
    carrier.ssb_center_freq_hz     = 3.5e9;
    carrier.offset_to_carrier      = 0;
    carrier.scs                    = (srsran_subcarrier_spacing_t)scs_enum;
    carrier.nof_prb                = bwp_prbs_;
    carrier.start                  = 0;
    carrier.max_mimo_layers        = 1;

    // ---- Init the PDSCH decoder ----
    srsran_pdsch_nr_args_t args = {};
    args.sch.disable_simd = false;
    args.measure_evm      = false;
    args.measure_time     = false;
    args.max_prb          = bwp_prbs_;
    args.max_layers       = 1;

    auto* pdsch = new srsran_pdsch_nr_t();
    *pdsch = {};
    if (srsran_pdsch_nr_init_ue(pdsch, &args) < SRSRAN_SUCCESS) {
        std::fprintf(stderr, "[pdsch] srsran_pdsch_nr_init_ue failed\n");
        return;
    }
    if (srsran_pdsch_nr_set_carrier(pdsch, &carrier) < SRSRAN_SUCCESS) {
        std::fprintf(stderr, "[pdsch] srsran_pdsch_nr_set_carrier failed\n");
        srsran_pdsch_nr_free(pdsch);
        delete pdsch;
        return;
    }
    pdsch_ = pdsch;

    // ---- RX soft-buffer (mandatory for LDPC decode) ----
    auto* softbuffer = new srsran_softbuffer_rx_t();
    *softbuffer = {};
    if (srsran_softbuffer_rx_init_guru(softbuffer, SRSRAN_SCH_NR_MAX_NOF_CB_LDPC,
                                       SRSRAN_LDPC_MAX_LEN_ENCODED_CB) < SRSRAN_SUCCESS) {
        std::fprintf(stderr, "[pdsch] softbuffer_rx_init failed\n");
        return;
    }
    softbuffer_ = softbuffer;

    // ---- Channel estimate result (identity channel) ----
    auto* chest = new srsran_chest_dl_res_t();
    *chest = {};
    if (srsran_chest_dl_res_init(chest, bwp_prbs_) < SRSRAN_SUCCESS) {
        std::fprintf(stderr, "[pdsch] chest_dl_res_init failed\n");
        return;
    }
    chest_ = chest;

    // ---- Slot resource grid (nof_prb * 12 * 14 complex) ----
    grid_ = new std::complex<float>[SRSRAN_SLOT_LEN_RE_NR(bwp_prbs_)];

    srsran_ready_ = true;
    if (verbose_) {
        std::printf("[pdsch] Pdsch: %.2f MHz, %u kHz, PCI %u, BWP %u PRB, decode=on (srsRAN-4G)\n",
                    sample_rate_ / 1e6, scs_hz_ / 1000u, (unsigned)pci_, (unsigned)bwp_prbs_);
    }
#endif
}

Pdsch::~Pdsch()
{
#ifdef GONE_HAVE_SRSRAN_OLD
    if (pdsch_) {
        srsran_pdsch_nr_free((srsran_pdsch_nr_t*)pdsch_);
        delete (srsran_pdsch_nr_t*)pdsch_;
    }
    if (softbuffer_) {
        srsran_softbuffer_rx_free((srsran_softbuffer_rx_t*)softbuffer_);
        delete (srsran_softbuffer_rx_t*)softbuffer_;
    }
    if (chest_) {
        srsran_chest_dl_res_free((srsran_chest_dl_res_t*)chest_);
        delete (srsran_chest_dl_res_t*)chest_;
    }
    delete[] grid_;
#endif
}

// ---------------------------------------------------------------- decode

#ifdef GONE_HAVE_SRSRAN_OLD

PdschTb Pdsch::run_decode(const std::complex<float>* grid, const DciFormat10& dci,
                          uint16_t rnti)
{
    PdschTb out;

    if (!srsran_ready_) {
        if (verbose_) std::fprintf(stderr, "[pdsch] not ready (srsRAN-4G init failed)\n");
        return out;
    }
    if (!dci.valid || dci.n_length_prb == 0) {
        return out;
    }

    auto*       pdsch      = (srsran_pdsch_nr_t*)pdsch_;
    auto*       softbuffer = (srsran_softbuffer_rx_t*)softbuffer_;
    auto*       chest      = (srsran_chest_dl_res_t*)chest_;

    // ---- Copy the caller's grid into our working grid ----
    std::memcpy(grid_, grid, SRSRAN_SLOT_LEN_RE_NR(bwp_prbs_) * sizeof(std::complex<float>));

    // ---- Build the SCH config + grant from the DCI ----
    srsran_sch_cfg_nr_t cfg = {};
    cfg.sch_cfg.mcs_table = srsran_mcs_table_64qam;

    // Data scrambling id: for RAR/CSS this is the cell id.
    cfg.scrambling_id_present = true;
    cfg.scambling_id          = pci_;

    // DMRS config (Type 1, 1-symbol, type-A pos 2, front-loaded).
    cfg.dmrs.type          = srsran_dmrs_sch_type_1;
    cfg.dmrs.length        = srsran_dmrs_sch_len_1;
    cfg.dmrs.additional_pos = srsran_dmrs_sch_add_pos_2;
    cfg.dmrs.typeA_pos     = srsran_dmrs_sch_typeA_pos_2;
    cfg.dmrs.scrambling_id0_present = true;
    cfg.dmrs.scrambling_id0         = pci_;
    cfg.dmrs.scrambling_id1_present = false;

    // Grant.
    srsran_sch_grant_nr_t& grant = cfg.grant;
    grant.rnti      = rnti;
    grant.rnti_type = srsran_rnti_type_ra;

    // Time domain: for a RAR (type A pos 2, row 0) PDSCH starts at symbol 2,
    // spans 12 symbols. We set mapping type A, S=2, L=12.
    grant.mapping = srsran_sch_mapping_type_A;
    grant.S       = 2;
    grant.L       = 12;

    // Frequency domain: RB allocation from the DCI RIV.
    std::memset(grant.prb_idx, 0, sizeof(grant.prb_idx));
    uint32_t n0 = dci.n_start_prb;
    uint32_t nl = dci.n_length_prb;
    for (uint32_t rb = 0; rb < nl; ++rb) grant.prb_idx[n0 + rb] = true;
    grant.nof_prb = nl;

    grant.nof_layers = 1;
    grant.nof_dmrs_cdm_groups_without_data = 1;
    grant.n_scid = false;
    grant.dci_format = srsran_dci_format_nr_1_0;
    grant.dci_search_space = srsran_search_space_type_common_1;

    // Fill the transport block (mcs, tbs, mod, nof_re, ...).
    if (srsran_ra_nr_fill_tb(&cfg, &grant, dci.mcs, &grant.tb[0]) != SRSRAN_SUCCESS) {
        if (verbose_) std::fprintf(stderr, "[pdsch] srsran_ra_nr_fill_tb failed\n");
        return out;
    }
    grant.tb[0].softbuffer.rx = softbuffer;
    srsran_softbuffer_rx_reset(softbuffer);

    // Output buffer for the TB payload.
    int tbs_bits = grant.tb[0].tbs;
    if (tbs_bits <= 0) return out;
    std::vector<uint8_t> payload((tbs_bits + 7) / 8, 0);

    srsran_pdsch_res_nr_t res = {};
    res.tb[0].payload = payload.data();

    // Identity channel estimate. Decode requires channel->nof_re to equal the
    // grant's DATA RE count (tb[0].nof_re), not the full slot size; the ce[] we
    // provide must cover exactly those data REs in get() order.
    uint32_t nof_data_re = (uint32_t)grant.tb[0].nof_re;
    for (uint32_t i = 0; i < nof_data_re; ++i) {
        chest->ce[0][0][i] = 1.0f;
    }
    chest->nof_re = nof_data_re;

    cf_t* sf_symbols[SRSRAN_MAX_PORTS] = {};
    sf_symbols[0] = (cf_t*)grid_;

    if (srsran_pdsch_nr_decode(pdsch, &cfg, &grant, chest, sf_symbols, &res) != SRSRAN_SUCCESS) {
        if (verbose_) std::fprintf(stderr, "[pdsch] srsran_pdsch_nr_decode error\n");
        return out;
    }

    if (!res.tb[0].crc) {
        if (verbose_) std::fprintf(stderr, "[pdsch] TB CRC mismatch\n");
        return out;
    }

    out.valid    = true;
    out.tbs_bits = (uint32_t)tbs_bits;
    out.tb_bytes.assign(payload.begin(), payload.end());
    return out;
}
#endif // GONE_HAVE_SRSRAN_OLD

PdschTb Pdsch::decode_grid(const std::complex<float>* grid, const DciFormat10& dci,
                           uint16_t rnti)
{
#ifdef GONE_HAVE_SRSRAN_OLD
    return run_decode(grid, dci, rnti);
#else
    (void)grid; (void)dci; (void)rnti;
    return PdschTb{};
#endif
}

PdschTb Pdsch::demodulate(const std::vector<Symbol>& symbols, uint32_t sym0,
                          const DciFormat10& dci, uint16_t rnti)
{
#ifdef GONE_HAVE_SRSRAN_OLD
    if (!srsran_ready_ || !dci.valid) return PdschTb{};

    // Build the slot resource grid from the demodulated symbols.
    // srsRAN grid index: (sym*nof_prb + rb)*12 + k. Our Symbol.samples has
    // bwp_prbs*12 subcarriers in RB order: samples[rb*12 + k].
    const uint32_t nsc = bwp_prbs_ * 12;
    std::memset(grid_, 0, SRSRAN_SLOT_LEN_RE_NR(bwp_prbs_) * sizeof(std::complex<float>));

    const uint32_t n0 = dci.n_start_prb;
    const uint32_t nl = dci.n_length_prb;
    const uint32_t S  = 2;      // type A pos 2
    const uint32_t L  = 12;

    for (uint32_t l = S; l < S + L; ++l) {
        uint32_t sym_idx = sym0 + l;
        if (sym_idx >= symbols.size()) break;
        const auto& sym = symbols[sym_idx];
        // Allocate only the DCI RBs (scatter data into the grid at PDSCH REs).
        // We place the received sample at every subcarrier of the allocated RBs;
        // srsRAN's decoder skips DMRS/reserved positions via its RE mask.
        for (uint32_t rb = 0; rb < nl; ++rb) {
            uint32_t prb = n0 + rb;
            if (prb >= bwp_prbs_) continue;
            for (uint32_t k = 0; k < 12; ++k) {
                uint32_t sc = prb * 12 + k;
                if (sc < sym.samples.size()) {
                    grid_[(l * bwp_prbs_ + prb) * 12 + k] = sym.samples[sc];
                }
            }
        }
    }

    return run_decode(grid_, dci, rnti);
#else
    (void)symbols; (void)sym0; (void)dci; (void)rnti;
    return PdschTb{};
#endif
}

} // namespace gone::nr
