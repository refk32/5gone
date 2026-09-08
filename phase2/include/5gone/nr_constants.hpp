#pragma once   // Prevents this header from being included twice in the same file.

/*
 * nr_constants.hpp
 * ================
 * Physical-layer constants and helpers for the 5Gone live RAR (Msg2) decoder.
 *
 * Think of this as a "cheat sheet" of hard numbers that the 5G NR standard
 * (3GPP TS 38.xxx) defines, plus the specific numbers for OUR lab cell.
 *
 * Everything here is derived from the lab gNB config:
 *     config/srsran/gnb_20mhz.yml
 *         sample_rate = 23.04 MHz
 *         common_scs  = 30 kHz   (subcarrier spacing -> numerology mu = 1)
 *         channel_bandwidth = 20 MHz
 *         pci = 1
 *
 * WHY do we need these numbers?
 *   - The OFDM demodulator needs FFT size + cyclic prefix lengths.
 *   - The PDCCH decoder needs the CORESET size and the DM-RS pattern.
 *   - The RAR search needs the RA-RNTI value range.
 */

#include <cstdint>   // for fixed-size integer types (uint8_t, uint16_t, ...)

// We put everything inside a "namespace" called gp/5g-nr.
// A namespace just groups names so they don't clash with other code.
namespace gone::nr {

// ---------------------------------------------------------------------------
// Resource-element & CORESET basics (TS 38.211 / 38.213)
// ---------------------------------------------------------------------------

// One PRB (physical resource block) = 12 subcarriers (frequency "columns").
static constexpr uint8_t PRB_RE = 12;

// A REG (resource element group) is 1 OFDM symbol x 1 PRB = 12 REs.
static constexpr uint8_t REG_RE = PRB_RE;

// One CCE (control channel element) = 6 REGs.
// PDCCH is made out of CCEs; this is how we know how big a piece of PDCCH is.
static constexpr uint8_t CCE_REG = 6;

// PDCCH puts its DM-RS (reference signal) on 3 of the 12 subcarriers per PRB.
static constexpr uint8_t DMRS_RE_PRB = 3;

// ...which means 18 DM-RS subcarriers per CCE (6 REGs * 3).
static constexpr uint8_t DMRS_SC_CCE = 18;

// Aggregation levels: a DCI can be carried by 1, 2, 4, 8 or 16 CCEs.
// We try each size during "blind" decoding.
static constexpr uint8_t NUM_ALs = 5;   // we have 5 possible sizes

static constexpr uint8_t AL_1  = 1;
static constexpr uint8_t AL_2  = 2;
static constexpr uint8_t AL_4  = 4;
static constexpr uint8_t AL_8  = 8;
static constexpr uint8_t AL_16 = 16;

// ---------------------------------------------------------------------------
// PN (pseudo-random / Gold) sequence (TS 38.211 5.2.1)
// ---------------------------------------------------------------------------
// Used to generate the DM-RS and to scramble PDCCH bits.
static constexpr uint16_t gold_sequence_length = 31;  // the two m-sequences
static constexpr uint16_t Nc = 1600;                  // standard offset constant

// ---------------------------------------------------------------------------
// Numerology (from the gNB config: 30 kHz subcarrier spacing)
// ---------------------------------------------------------------------------
// "Numerology" mu determines the subcarrier spacing:  SCS = 15 kHz * 2^mu.
// mu = 1  ->  30 kHz.  Everything below follows from this.
static constexpr uint8_t  numerology       = 1;
static constexpr uint32_t scs_hz           = 30000;   // 30 kHz

// A slot always has 14 OFDM symbols (normal cyclic prefix).
static constexpr uint32_t symbols_per_slot = 14;

// Number of slots per 10 ms radio frame = 10 * 2^mu = 20.
static constexpr uint32_t slots_per_frame  = 10 * (1u << numerology);

// Number of slots per 1 ms subframe = 2^mu = 2.
static constexpr uint32_t slots_per_subframe = (1u << numerology);

// Symbols per subframe = 2 slots * 14 symbols = 28.
static constexpr uint32_t symbols_per_subframe = slots_per_subframe * symbols_per_slot;

// ---------------------------------------------------------------------------
// Active bandwidth part (BWP) for 20 MHz @ 30 kHz
// ---------------------------------------------------------------------------
// The BWP is the slice of spectrum the gNB actually uses for data/PDCCH.
// For 20 MHz @ 30 kHz SCS this is 51 PRBs (TS 38.101 Table 5.3.2-1).
static constexpr uint16_t bwp_num_prbs = 51;

// ---------------------------------------------------------------------------
// Timing helpers (used to compute slot/FET durations)
// ---------------------------------------------------------------------------
static constexpr double seconds_per_subframe = 0.001;   // 1 ms
static constexpr double Tc = 1.0 / (480000.0 * 4096.0); // basic 5G time unit
static constexpr double K  = 64.0;                      // scaling constant

// ---------------------------------------------------------------------------
// RA-RNTI range to blind-search for RAR scheduling DCIs (TS 38.321 5.1.3)
// ---------------------------------------------------------------------------
//
// When a phone does a random access (Msg1/PRACH), the gNB answers with a
// "Random Access Response" (RAR, aka Msg2). It schedules that RAR on PDCCH
// using a special ID called the RA-RNTI. The RA-RNTI value is computed from
// the time & frequency where the phone's PRACH was detected:
//
//   RA-RNTI = 1 + s_id + 14*t_id + 14*80*f_id + 14*80*8*ul_carrier_id
//
//   s_id : first OFDM symbol of PRACH (0..13)
//   t_id : first slot of PRACH (0..79)
//   f_id : frequency index (0..7)
//
// For a small cell the value stays well below 71, so we blind-search 1..71.
static constexpr uint16_t ra_rnti_min = 1;
static constexpr uint16_t ra_rnti_max = 71;

} // namespace gone::nr
