#include "5gone/nr_constants.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_symbol.hpp"
#include "5gone/nr_pdsch.hpp"
#include "5gone/mac_rar.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <vector>

#ifdef GONE_HAVE_SRSRAN_OLD
// Old srsRAN 4G C API used here ONLY for the transmit side (encode a RAR into
// a slot grid). The receive side goes through our Pdsch::demodulate() which is
// the exact path the live decoder uses.
#include <srsran/srsran.h>
#include <srsran/phy/fec/softbuffer.h>
#include <srsran/phy/phch/pdsch_nr.h>
#include <srsran/phy/phch/ra_nr.h>
#endif

using namespace gone::nr;

// ---------------------------------------------------------------------------
// test_rar_pdsch.cpp
// ---------------------------------------------------------------------------
// Full RAR round trip (Step 7):
//   1. Hand-build a MAC RAR subPDU (RAPID + TA + Temp C-RNTI + UL grant).
//   2. srsRAN_4G PDSCH *encode* maps those TB bytes into a 51-PRB slot grid
//      (LDPC rate 1/3, CRC, scramble, QAM, RE mapping) — exactly the srsRAN
//      TX objects the gNB uses.
//   3. Convert the grid to Symbols, IFFT them with our Ofdm::modulate(), then
//      FFT back with Ofdm::demodulate() (the same signal chain the receiver
//      sees end-to-end).
//   4. Feed the symbols into Pdsch::demodulate() — the real live path — and
//      recover the transport block.
//   5. parse_mac_rar() must pull back the exact RAPID/TA/TC-RNTI/UL-grant.
//
// If this passes, the live downlink decode chain (RarDecoder) has a working
// PDSCH+MAC layer verified against a transmitted-by-srsRAN signal.

static int g_fails = 0;

static void check(bool cond, const char* what)
{
    std::printf("%s: %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond) ++g_fails;
}

int main()
{
    const double  sample_rate  = 23.04e6;
    const double  scs          = 30000.0;
    constexpr uint16_t pci     = 1;
    constexpr uint32_t nprb    = bwp_num_prbs;   // 51
    constexpr uint16_t rnti    = 12;             // arbitrary RA-RNTI

    // ---- 1. Build the MAC RAR subPDU (8 bytes: 1 subheader + 7 body) ----
    // Subheader layout (TS 38.321 6.2.3): E(1) T(1=RAPID) RAPID(6).
    // Body (from srsRAN's mac_rar_pdu_nr.cc):
    //   b0: TA high 7 bits (b7 reserved, must be 0)
    //   b1: TA low 5 bits | UL-grant bits 0..2
    //   b2..b4: UL-grant bits 3..19 (MSB first), lower bits padding
    //   b5 b6: Temp C-RNTI (big endian)
    constexpr  uint8_t  rapid    = 7;
    constexpr  uint16_t ta       = 0x123;   // 291
    constexpr  uint16_t tc_rnti  = 0x1357;
    constexpr  uint32_t ug       = 0xB60D1; // 20-bit RAR UL grant value

    std::array<uint8_t, 8> msc_pdu = {};
    msc_pdu[0] = (uint8_t)((1u << 6) | (rapid & 0x3f));      // T=1(RAPID), E=0
    msc_pdu[1] = (uint8_t)((ta >> 5) & 0x7f);                 // TA high 7 bits
    // TA low 5 bits (bits 7..3) + UL-grant bits 0..2 (bits 2..0).
    msc_pdu[2] = (uint8_t)(((ta & 0x1f) << 3) |
                           ((ug >> 0) & 1u) << 2 |
                           ((ug >> 1) & 1u) << 1 |
                           ((ug >> 2) & 1u));
    // UL-grant bits 3..19 -> bytes 3..5, MSB first (matches srsRAN decoder).
    for (int b = 3; b <= 19; ++b) {
        int byte = 3 + (b - 3) / 8;
        int bit  = 7 - ((b - 3) % 8);
        if ((ug >> b) & 1u) msc_pdu[byte] |= (uint8_t)(1u << bit);
    }
    msc_pdu[6] = (uint8_t)(tc_rnti >> 8);
    msc_pdu[7] = (uint8_t)(tc_rnti & 0xff);

    std::printf("[test] MAC RAR PDU bytes: ");
    for (uint8_t b : msc_pdu) std::printf("%02X ", (unsigned)b);
    std::printf("\n");

#ifdef GONE_HAVE_SRSRAN_OLD
    // ---- 2a. srsRAN TX objects (mirror run_decode() config exactly) ----
    srsran_carrier_nr_t carrier = {};
    carrier.pci                    = pci;
    carrier.dl_center_frequency_hz = 3.5e9;
    carrier.ul_center_frequency_hz = 3.5e9;
    carrier.ssb_center_freq_hz     = 3.5e9;
    carrier.offset_to_carrier      = 0;
    carrier.scs                    = srsran_subcarrier_spacing_30kHz;
    carrier.nof_prb                = nprb;
    carrier.start                  = 0;
    carrier.max_mimo_layers        = 1;

    srsran_pdsch_nr_args_t args_tx = {};
    args_tx.sch.disable_simd = false;
    args_tx.max_prb          = nprb;
    args_tx.max_layers       = 1;

    srsran_pdsch_nr_t pdsch_tx = {};
    check(srsran_pdsch_nr_init_enb(&pdsch_tx, &args_tx) == SRSRAN_SUCCESS, "pdsch_nr_init_enb()");
    if (srsran_pdsch_nr_set_carrier(&pdsch_tx, &carrier) != SRSRAN_SUCCESS) {
        std::printf("FAIL: pdsch_nr_set_carrier (tx)\n");
        ++g_fails;
    }

    srsran_softbuffer_tx_t soft_tx = {};
    check(srsran_softbuffer_tx_init_guru(&soft_tx, SRSRAN_SCH_NR_MAX_NOF_CB_LDPC,
                                         SRSRAN_LDPC_MAX_LEN_ENCODED_CB) == SRSRAN_SUCCESS,
          "softbuffer_tx_init_guru()");

    std::vector<cf_t> grid(SRSRAN_SLOT_LEN_RE_NR(nprb), 0.0f);
    const auto* grid_f = reinterpret_cast<const std::complex<float>*>(grid.data());

    // ---- 2b. SCH config + grant ----
    srsran_sch_cfg_nr_t cfg = {};
    cfg.sch_cfg.mcs_table = srsran_mcs_table_64qam;

    cfg.scrambling_id_present = true;
    cfg.scambling_id          = pci;

    cfg.dmrs.type            = srsran_dmrs_sch_type_1;
    cfg.dmrs.length          = srsran_dmrs_sch_len_1;
    cfg.dmrs.additional_pos  = srsran_dmrs_sch_add_pos_2;
    cfg.dmrs.typeA_pos       = srsran_dmrs_sch_typeA_pos_2;
    cfg.dmrs.scrambling_id0_present = true;
    cfg.dmrs.scrambling_id0         = pci;
    cfg.dmrs.scrambling_id1_present = false;

    srsran_sch_grant_nr_t& grant = cfg.grant;
    grant.rnti      = rnti;
    grant.rnti_type = srsran_rnti_type_ra;
    grant.mapping   = srsran_sch_mapping_type_A;
    grant.S         = 2;
    grant.L         = 12;

    const uint32_t n0 = 0;   // DCI tells us RB start
    const uint32_t nl = 6;   // ... and length (6 PRBs for one short RAR)
    std::memset(grant.prb_idx, 0, sizeof(grant.prb_idx));
    for (uint32_t rb = 0; rb < nl; ++rb) grant.prb_idx[n0 + rb] = true;
    grant.nof_prb = nl;

    grant.nof_layers = 1;
    grant.nof_dmrs_cdm_groups_without_data = 1;
    grant.n_scid = false;
    grant.dci_format = srsran_dci_format_nr_1_0;
    grant.dci_search_space = srsran_search_space_type_common_1;

    const int mcs = 4;
    check(srsran_ra_nr_fill_tb(&cfg, &grant, mcs, &grant.tb[0]) == SRSRAN_SUCCESS, "ra_nr_fill_tb()");
    grant.tb[0].softbuffer.tx = &soft_tx;
    srsran_softbuffer_tx_reset(&soft_tx);

    const int tbs_bits = grant.tb[0].tbs;
    const int tbs_bytes = (tbs_bits + 7) / 8;
    std::printf("[test] mcs=%d 6PRB TB: %d bits = %d bytes (need >= 8)\n", mcs, tbs_bits, tbs_bytes);
    if (tbs_bytes < 8) { std::printf("FAIL: TB too small for a RAR\n"); ++g_fails; }

    std::vector<uint8_t> sch_payload(tbs_bytes, 0);
    std::memcpy(sch_payload.data(), msc_pdu.data(), 8);

    // ---- 2c. Encode into the grid ----
    uint8_t* data[SRSRAN_MAX_TB] = {sch_payload.data(), nullptr};
    cf_t*    sf_symbols[SRSRAN_MAX_PORTS] = {};
    sf_symbols[0] = grid.data();
    check(srsran_pdsch_nr_encode(&pdsch_tx, &cfg, &grant, data, sf_symbols) == SRSRAN_SUCCESS,
          "pdsch_nr_encode()");

    // ---- 3. grid -> Symbols -> IFFT -> FFT (your receiver's signal chain) ----
    Ofdm ofdm(sample_rate, (uint32_t)scs, nprb);
    const uint32_t nsc = nprb * 12;        // 612 subcarriers
    const uint32_t half = nsc / 2;         // 306, matches Ofdm's fft-shift layout

    std::vector<Symbol> tx;
    for (uint32_t l = 0; l < 14; ++l) {
        Symbol s;
        s.samples.resize(nsc);
        for (uint32_t rb = 0; rb < nprb; ++rb) {
            for (uint32_t k = 0; k < 12; ++k) {
                // Grid subcarrier sc sits at FFT bin sc = sample sc (identity).
                s.samples[rb * 12 + k] = grid_f[(l * nprb + rb) * 12 + k];
            }
        }
        s.symbol_index = (uint8_t)l;
        s.slot_index   = 0;
        tx.push_back(std::move(s));
    }

    // Transmit + receive: modulate -> time IQ -> demodulate -> symbols.
    std::vector<std::complex<float>> iq = ofdm.modulate(tx);
    std::vector<Symbol> rx = ofdm.demodulate(iq);
    check(rx.size() == 14, "modulate/demodulate returns 14 symbols");

    // ---- 4. Decode the PDSCH through the LIVE path ----
    Pdsch pdsch(sample_rate, (uint32_t)scs, pci, nprb, false);

    DciFormat10 dci;
    dci.valid       = true;
    dci.n_start_prb = n0;
    dci.n_length_prb = nl;
    dci.mcs         = (uint8_t)mcs;
    dci.harq_process_number = 0;

    PdschTb tb = pdsch.demodulate(rx, 0, dci, rnti);
    check(tb.valid, "PDSCH decode (TB CRC pass)");
    if (tb.valid) {
        std::printf("[test] recovered TB: ");
        for (uint8_t b : tb.tb_bytes) std::printf("%02X ", (unsigned)b);
        std::printf("\n");
    }

    // ---- 5. MAC RAR parse must match what we built ----
    bool mac_ok = false, rap_ok = false, ta_ok = false, tc_ok = false, ug_ok = false;
    if (tb.valid) {
        MacRar rar = parse_mac_rar(tb.tb_bytes);
        mac_ok = rar.valid;
        rap_ok = rar.valid && (rar.rapid == rapid);
        ta_ok  = rar.valid && (rar.timing_advance == ta);
        tc_ok  = rar.valid && (rar.t_c_rnti == tc_rnti);
        if (rar.valid) {
            ug_ok = true;
            for (int b = 0; b < 20; ++b) {
                bool txb = (ug >> b) & 1u;
                bool rxb = ((uint32_t)(rar.ul_grant[b] ? 1 : 0)) != 0;
                if (txb != rxb) { ug_ok = false; break; }
            }
        }
        std::printf("[test] decoded RAR: RAPID=%u TA=%u TC-RNTI=0x%04X UL-grant-bytes=",
                    rar.valid ? (unsigned)rar.rapid : 0,
                    rar.valid ? (unsigned)rar.timing_advance : 0,
                    rar.valid ? (unsigned)rar.t_c_rnti : 0);
        if (rar.valid) {
            for (int b = 0; b < 20; ++b) std::printf("%u", (unsigned)rar.ul_grant[b]);
            std::printf("\n");
        } else {
            std::printf("(parse fail)\n");
        }
    }
    check(mac_ok, "MAC RAR parsed");
    check(rap_ok, "RAPID matches");
    check(ta_ok,  "Timing advance matches");
    check(tc_ok,  "Temp C-RNTI matches");
    check(ug_ok,  "UL grant matches");

    srsran_softbuffer_tx_free(&soft_tx);
    srsran_pdsch_nr_free(&pdsch_tx);
#else
    (void)msc_pdu;
    std::printf("SKIP: built without srsRAN-4G (GONE_HAVE_SRSRAN_OLD not defined)\n");
#endif

    std::printf("done: %d failure(s)\n", g_fails);
    return g_fails == 0 ? 0 : 1;
}