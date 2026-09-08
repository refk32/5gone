// nr_pdcch_decode_srsran.ipp
// ==========================
// The polar + CRC decode half of PDCCH decoding. This is included by
// nr_pdcch.cpp's Pdcch::decode_pdcch() ONLY when GONE_HAVE_SRSRAN_OLD is
// defined (i.e. when building against the OLD srsRAN 4G C API, the same one
// 5GSniffer vendors in lib/srsRANRF). Without that define this file is never
// compiled, so phase2 still builds on machines without srsRAN_4G.
//
// This is a faithful port of 5GSniffer's pdcch::decode_pdcch (pdcch.cc).
// It turns the equalized, channel-corrected QPSK symbols of one candidate into
// the 39 decoded DCI bits - or decides the CRC failed (wrong candidate/RNTI).
//
// Variables already in scope when this is included (from decode_pdcch()):
//   Symbol&  symbol;          vector<complex<float>>& pdcch_symbols;
//   Dci&     dci_  (the candidate, .nof_bits set = payload size; .crc_ok output)
//   bool     rep_opt;         int64_t metadata;       int symbol_in_chunk;
// plus class members (found_rnti_list_, coreset_info.cell_id, sample_rate_time).

#ifndef GONE_HAVE_SRSRAN_OLD
#error "nr_pdcch_decode_srsran.ipp must only be included with GONE_HAVE_SRSRAN_OLD"
#endif

// NOTE: <srsran/srsran.h> is intentionally NOT included here — this file is
// textually included INSIDE a function body, and srsran.h opens with
// `extern "C" {`, which is only valid at file scope. Include it from the top
// of nr_pdcch.cpp (guarded by #ifdef GONE_HAVE_SRSRAN_OLD) instead.

#include <cstdlib>

{
  // srsRAN's PDCCH NR receiver works on a state object `q` that we initialize
  // once per decode attempt. We configure it for our fixed lab numerology.
  srsran_pdcch_nr_args_t args = {};
  args.disable_simd = false;
  args.measure_evm  = false;
  args.measure_time = false;

  srsran_pdcch_nr_t q = {};
  if (srsran_pdcch_nr_init_rx(&q, &args) < SRSRAN_SUCCESS) {
    dci_.crc_ok = false;
    return 0;
  }

  // Size of the code words, exactly as in 5GSniffer:
  //   K = payload + 24 CRC bits
  //   M = number of resource elements the candidate spans
  //   E = M*2 because QPSK carries 2 bits per RE (rate-matched bits)
  q.K = dci_.nof_bits + 24U;                                  // e.g. 39+24 = 63
  q.M = dci_.found_aggregation_level * (PRB_RE - 3U) * CCE_REG;
  q.E = q.M * 2;

  // Get the polar code parameters for this (K, E).
  if (srsran_polar_code_get(&q.code, q.K, q.E, 9U) < SRSRAN_SUCCESS) {
    dci_.crc_ok = false;
    srsran_pdcch_nr_free(&q);
    return 0;
  }

  // 1) Soft demodulate QPSK symbols -> LLRs (log-likelihood ratios).
  //    The LLR buffer is q.f.
  int8_t* llr = (int8_t*)q.f;
  srsran_demod_soft_demodulate_b(SRSRAN_MOD_QPSK, (const cf_t*)pdcch_symbols.data(),
                                 llr, q.M);

  // 2) 5GSniffer negates all LLRs (its convention flips the sign).
  for (uint32_t i = 0; i < q.E; i++) {
    llr[i] *= -1;
  }

  // 3) Repetition optimization (only for high AL where E > N, i.e. the polar
  //    mother code is shorter than the rate-matched output so symbols repeat).
  //    Then a repeating pattern hints at the RNTI without full decoding.
  if (rep_opt) {
    int N_length = 1 << q.code.n;
    float total_sum = 0.0f;
    float max_value = 0.0f;
    int   max_pos   = -1;
    if ((int)q.E > N_length) {
      int8_t* llr_aux = (int8_t*)malloc(q.E * sizeof(int8_t));
      for (auto N_RNTI : found_rnti_list_) {
        srsran_sequence_apply_c(llr, llr_aux, q.E, pdcch_nr_c_init_scrambler(N_RNTI, dci_.pdcch_scrambling_id));
        int sum = 0;
        for (int i = 0; i < ((int)q.E - N_length); i++) {
          sum += std::abs(llr_aux[i] + llr_aux[i + N_length]);
        }
        total_sum += (float)sum;
        if (sum > max_value) { max_value = (float)sum; max_pos = N_RNTI; }
      }
      if (max_value > 1.05f * (total_sum / (float)found_rnti_list_.size())) {
        dci_.rnti = (uint16_t)max_pos;
      }
      free(llr_aux);
    }
  }

  // 4) Descrambling. For a Common Search Space (RA-RNTI) the PDCCH is scrambled
  //    with rnti=0 and the DM-RS scrambling id = cell id (TS 38.211 7.3.2.3).
  //    We only take the RNTI-scrambled path when an explicit RNTI was requested
  //    AND the DM-RS scrambling id differs from the cell id.
  if ((dci_.rnti < 65520 && dci_.rnti > 100) &&
      dci_.pdcch_scrambling_id != coreset_info.cell_id) {
    srsran_sequence_apply_c(llr, llr, q.E, pdcch_nr_c_init_scrambler(dci_.rnti, dci_.pdcch_scrambling_id));
  } else {
    srsran_sequence_apply_c(llr, llr, q.E, pdcch_nr_c_init_scrambler(0, coreset_info.cell_id));
  }

  // 5) Un-rate-match the LLR stream back to the polar mother code length.
  uint8_t PDCCH_NR_POLAR_RM_IBIL = 0;
  int8_t* d = (int8_t*)q.d;
  if (srsran_polar_rm_rx_c(&q.rm, llr, d, q.E, q.code.n, q.K, PDCCH_NR_POLAR_RM_IBIL) < SRSRAN_SUCCESS) {
    dci_.crc_ok = false;
    srsran_pdcch_nr_free(&q);
    return 0;
  }

  // 6) Polar decode.
  if (srsran_polar_decoder_decode_c(&q.decoder, d, q.allocated, q.code.n,
                                    q.code.F_set, q.code.F_set_size) < SRSRAN_SUCCESS) {
    dci_.crc_ok = false;
    srsran_pdcch_nr_free(&q);
    return 0;
  }

  // 7) De-allocate (remove the frozen/punctured positions) -> c_prime.
  //    SRSRAN_POLAR_INTERLEAVER_K_MAX_IL is a macro defined by polar_interleaver.h
  //    (TS 38.212 5.3.1.1); use it directly so we don't shadow the macro.
  uint8_t c_prime[SRSRAN_POLAR_INTERLEAVER_K_MAX_IL];
  srsran_polar_chanalloc_rx(q.allocated, c_prime, q.code.K, q.code.nPC,
                            q.code.K_set, q.code.PC_set);

  // 8) The first 24 bits are the CRC (set to ones by the transmitter).
  uint8_t* c = q.c;
  srsran_bit_unpack(UINT32_MAX, &c, 24U);

  // 9) De-interleave to recover the original bit order.
  srsran_polar_interleaver_run(c_prime, c, (uint32_t)sizeof(uint8_t), q.K, false);

  // 10) De-scramble the CRC with the RNTI being tested (TS 38.212 7.3.2).
  uint8_t unpacked_rnti[16] = {};
  uint8_t* ptr = unpacked_rnti;
  srsran_bit_unpack(dci_.rnti, &ptr, 16);
  srsran_vec_xor_bbb(unpacked_rnti, &c[q.K - 16], &c[q.K - 16], 16);

  // 11) CRC check: recompute over the K bits and compare with the stored CRC.
  ptr = &c[q.K - 24];
  uint32_t checksum1 = srsran_crc_checksum(&q.crc24c, q.c, q.K);
  uint32_t checksum2 = srsran_bit_pack(&ptr, 24);
  bool ok = (checksum1 == checksum2);

  dci_.crc_ok = ok;

  if (ok) {
    // Copy the decoded payload bits (MSB first) into the Dci.
    dci_.payload.assign(c, c + dci_.nof_bits);
    // Move this RNTI to the front of the candidate list (heuristic from
    // 5GSniffer so we try the most-recently-seen RNTIs first).
    auto it = std::find(found_rnti_list_.begin(), found_rnti_list_.end(), dci_.rnti);
    if (it != found_rnti_list_.end()) {
      found_rnti_list_.erase(it);
      found_rnti_list_.insert(found_rnti_list_.begin(), dci_.rnti);
    }
  }

  srsran_pdcch_nr_free(&q);
  return ok ? 1 : 0;
}
