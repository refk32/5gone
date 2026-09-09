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

} // namespace

RarSlotTx build_rar_slot(Ofdm& ofdm, uint16_t pci, uint16_t bwp_prbs,
                         float payload_amp)
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

  // Symbol 0: RAR PDCCH DM-RS (AL 8, candidate 0, our own detector's reference)
  //           + QPSK "PDCCH data" everywhere else.
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

    constexpr uint8_t AL = 8, cand = 0, num_cands = 8, slot_index = 0;
    auto sc = pdcch.get_dmrs_sc_indices(AL, cand, num_cands, slot_index, false);
    auto seq = pdcch.get_dmrs_symbols(AL, cand, num_cands, slot_index, 0);
    const size_t ndmrs = sc.size() < seq.size() ? sc.size() : seq.size();

    std::vector<bool> is_dmrs(nsc, false);
    for (size_t i = 0; i < ndmrs; ++i) {
      if (sc[i] < nsc) { slot[0].samples[sc[i]] = seq[i]; is_dmrs[sc[i]] = true; }
    }
    for (uint16_t k = 0; k < nsc; ++k)
      if (!is_dmrs[k]) slot[0].samples[k] = ref_payload(k, 0, payload_amp);
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

} // namespace gone::nr