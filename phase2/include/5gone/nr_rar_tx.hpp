#pragma once

#include "5gone/nr_symbol.hpp"

#include <complex>
#include <cstdint>
#include <vector>

namespace gone::nr {

class Ofdm;

/*
 * nr_rar_tx.hpp
 * =============
 * A SYNTHETIC downlink RAR slot for the software-victim collision test
 * (--mode collide). We have no real gNB RF to attack, so we build the
 * victim's view from first principles with code we already own:
 *
 *   slot symbol 0: RAR PDCCH DM-RS (same Pdcch config the RarDecoder uses,
 *                 so our own detector correlates it) + a QPSK "PDCCH data"
 *                 region filling the non-DMRS subcarriers.
 *   slot symbol 1: guard symbol (empty).
 *   slot symbols 2..13: constant-modulus "message-2 / PDSCH payload" QPSK
 *                 reference region (stand-in for the actual RAR PDSCH the
 *                 victim would decode). Symbols 2/4 additionally carry the
 *                 SSB SSS/PSS so the slot stays burst-locatable.
 *
 * The ATTACK slot is the same SSB + PDCCH DMRS, but the message-2 payload is
 * phase-flipped 180 degrees: a hijacker broadcasting a *conflicting* RAR. When
 * superposed on the legit RAR the victim's message-2 clarity collapses / flips
 * sign while an extra RAR-PDCCH observation appears — the DoS we demonstrate.
 *
 * This is deliberately receive-side "reference" content (no real polar/CRC),
 * which is all the demo needs: the victim metric is RAR-PDCCH DM-RS correlation
 * + per-symbol message-2 frequency-domain clarity against the known grid.
 */

// Reference content for RX2 B (legit) and TX (legit + attack) built at once.
struct RarSlotTx {
  std::vector<std::complex<float>> legit;    // the gNB RAR slot (time domain)
  std::vector<std::complex<float>> attack;   // conflicting copy (payload flipped)

  // Legit frequency-domain grid (14 symbols x num_subcarriers). The victim
  // correlates each demodulated slot symbol against this reference; under an
  // integer-symbol attack the overlapped symbols flip sign (clarity -> -1).
  std::vector<Symbol> grid;

  // Start sample of each slot symbol (0..13) inside `legit`, plus slot length.
  std::vector<size_t> symbol_offsets;
  size_t slot_samples{0};
};

// Build a 14-symbol RAR slot. `payload_amp` = QPSK region amplitude (PSS/SSS
// and DM-RS keep their native unit amplitude). attack = legit with the msg2
// payload negated (same SSB/PDCCH, opposite payload).
RarSlotTx build_rar_slot(Ofdm& ofdm, uint16_t pci, uint16_t bwp_prbs,
                         float payload_amp = 0.5f);

} // namespace gone::nr