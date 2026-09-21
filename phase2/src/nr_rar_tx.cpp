#include "5gone/nr_rar_tx.hpp"

#include "5gone/nr_coreset.hpp"
#include "5gone/nr_ofdm.hpp"
#include "5gone/nr_pdcch.hpp"
#include "5gone/nr_pss.hpp"
#include "5gone/nr_symbol.hpp"

#include <cmath>
#include <cstdint>
#include <stdexcept>

namespace gone::nr {

namespace {

constexpr double kGoldenAngle = 2.399963229728653;  // QPSK-ish pseudo-random fill

// Deterministic constant-modulus payload for subcarrier `sc` in slot symbol `sym`.
std::complex<float> ref_payload(uint16_t sc, uint16_t sym, float amp)
{
  const double ph = static_cast<double>(sc) * kGoldenAngle +
                    static_cast<double>(sym) * 1.271828182845904;
  return amp * std::complex<float>(std::cos(ph), std::sin(ph));
}

// Shared body: fills the 14-symbol slot grid exactly like build_rar_slot, with
// the PDCCH DM-RS placed per `pdcch` (whose CORESET the caller configured).
// slot_index selects the DM-RS scrambling slot (default 0 preserves the old
// single-slot behaviour; tests synthesizing multi-slot captures pass the
// slot each burst will be scanned at).
RarSlotTx build_rar_slot_with_pdcch(Ofdm& ofdm, uint16_t pci, uint16_t bwp_prbs,
                                     float payload_amp, Pdcch& pdcch,
                                     uint8_t slot_index = 0)
{
  const uint8_t n_id2 = static_cast<uint8_t>(pci % 3);
  const uint8_t n_id1 = static_cast<uint8_t>(pci / 3);

  // --- Legit slot grid (14 symbols x 12*bwp_prbs subcarriers) ---
  std::vector<Symbol> slot(14);
  const uint32_t nsc = ofdm.num_subcarriers();
  for (auto& s : slot) {
    s.slot_index = 0;
    s.samples.assign(nsc, std::complex<float>(0.0f, 0.0f));
  }

  // Symbol 0..duration-1: RAR PDCCH DM-RS (AL 8 == coreset candidates[3],
  // candidate 0) + QPSK "PDCCH data" everywhere else on those symbols, at the
  // DM-RS locations the configured CORESET dictates. Supports CORESET durations
  // 1..3 (each OFDM symbol of the CORESET carries its own DM-RS slice).
  {
    constexpr uint16_t AL = 8;
    const uint8_t num_cands = (pdcch.coreset_info.candidates_search_space.size() > 3)
                                  ? pdcch.coreset_info.candidates_search_space[3] : 8;
    constexpr uint8_t cand = 0;
    const uint8_t dur = pdcch.coreset_info.duration ? pdcch.coreset_info.duration : 1;
    const uint8_t sym0 = pdcch.coreset_info.starting_ofdm_symbol_within_slot;
    auto sc = pdcch.get_dmrs_sc_indices(AL, cand, num_cands, slot_index, false);
    auto seq = pdcch.get_dmrs_symbols(AL, cand, num_cands, slot_index, 0);
    const size_t ndmrs = sc.size();
    const size_t per_sym = dur ? seq.size() / dur : 0;

    for (uint8_t di = 0; di < dur; ++di) {
      const size_t si = sym0 + di;
      if (si >= slot.size() || per_sym == 0) break;
      Symbol& out = slot[si];
      std::vector<bool> is_dmrs(nsc, false);
      const size_t n = (ndmrs < per_sym) ? ndmrs : per_sym;
      for (size_t i = 0; i < n; ++i) {
        const size_t idx = sc[i];
        if (idx < nsc) { out.samples[idx] = seq[static_cast<size_t>(di) * per_sym + i]; is_dmrs[idx] = true; }
      }
      for (uint16_t k = 0; k < nsc; ++k)
        if (!is_dmrs[k]) out.samples[k] = ref_payload(k, static_cast<uint16_t>(si), payload_amp);
    }
  }

  // Symbol 1: guard (empty).

  // Symbols 2..13: message-2 (PDSCH) payload; PSS/SSS ride in symbols 4/2.
  // The place_pss/place_sss calls run AFTER the payload fill and overwrite
  // subcarriers 56..182 with the unit-amplitude sequences.
  for (uint16_t sym = 2; sym <= 13; ++sym) {
    for (uint16_t k = 0; k < nsc; ++k)
      slot[sym].samples[k] = ref_payload(k, sym, payload_amp);
    if (sym == kPssSlotSymbol)   place_pss_in_symbol(slot[sym].samples, n_id2);
    if (sym == kSssSlotSymbol)   place_sss_in_symbol(slot[sym].samples, n_id1, n_id2);
  }

  // Build attack grid: same symbol 0 (PDCCH DM-RS) and SSB, but the message-2
  // payload is 180 degrees out of phase (a *conflicting* RAR from a hijacker).
  std::vector<Symbol> attack_slot = slot;
  for (uint16_t sym = 2; sym <= 13; ++sym)
    for (auto& v : attack_slot[sym].samples) v = -v;

  // --- Time domain ---
  RarSlotTx tx;
  tx.legit = ofdm.modulate(slot);
  tx.attack = ofdm.modulate(attack_slot);
  tx.grid = std::move(slot);      // victim's per-symbol reference

  // --- Symbol offsets (slot symbols 0..13 map to subframe symbols 0..13) ---
  tx.symbol_offsets.reserve(14);
  size_t off = 0;
  for (uint32_t l = 0; l < 14; ++l) {
    tx.symbol_offsets.push_back(off);
    off += ofdm.sym_len(l);
  }
  tx.slot_samples = ofdm.samples_per_slot();

  if (tx.legit.size() != tx.slot_samples || tx.attack.size() != tx.slot_samples)
    throw std::runtime_error("build_rar_slot: modulate length != samples_per_slot");
  if (tx.grid.size() != 14)
    throw std::runtime_error("build_rar_slot: grid must have 14 symbols");

  return tx;
}

} // namespace

RarSlotTx build_rar_slot(Ofdm& ofdm, uint16_t pci, uint16_t bwp_prbs,
                         float payload_amp)
{
  Pdcch pdcch;
  Coreset cs;
  cs.frequency_domain_resources = bwp_prbs;
  cs.duration = 1;
  cs.cell_id = pci;
  cs.starting_ofdm_symbol_within_slot = 0;
  cs.num_symbols_per_slot = 14;
  cs.num_slots_per_frame = 20;
  cs.candidates_search_space = {1, 2, 4, 8, 16};
  pdcch.set_coreset_info(cs);
  pdcch.scrambling_id_start = pci;
  pdcch.scrambling_id_end = pci;
  return build_rar_slot_with_pdcch(ofdm, pci, bwp_prbs, payload_amp, pdcch);
}

RarSlotTx build_rar_slot(Ofdm& ofdm, uint16_t pci, uint16_t bwp_prbs,
                         float payload_amp, const Coreset& cs,
                         uint8_t slot_index)
{
  Pdcch pdcch;
  pdcch.set_coreset_info(cs);
  pdcch.scrambling_id_start = pci;
  pdcch.scrambling_id_end = pci;
  return build_rar_slot_with_pdcch(ofdm, pci, bwp_prbs, payload_amp, pdcch,
                                   slot_index);
}

} // namespace gone::nr