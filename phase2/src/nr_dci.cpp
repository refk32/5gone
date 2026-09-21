#include "5gone/nr_dci.hpp"

#include <cmath>

namespace gone::nr {

// Number of bits in the frequency-domain resource assignment.
// Type-1 RIV needs enough bits to represent any contiguous allocation:
//   N(N+1)/2 possible values -> ceil(log2(N(N+1)/2)) bits.
uint32_t dci_format10_bits(uint32_t n_rb_bwp)
{
    return 1 + static_cast<uint32_t>(std::ceil(std::log2(
               static_cast<double>(n_rb_bwp) * (n_rb_bwp + 1) / 2.0)))
           + 4 + 1 + 5 + 1 + 2 + 4 + 2 + 2 + 3 + 3;
}

void riv_decode(uint32_t riv, uint32_t n_rb_bwp, uint32_t& n_start_prb, uint32_t& n_length_prb)
{
    // TS 38.214 5.1.2.2.1 (Type-1). The encoding was:
    //   if (L-1) <= floor(N/2) :  RIV = N*(L-1) + RB_start
    //   else                   :  RIV = N*(N-L+1) + (N-1-RB_start)
    //
    // Decoding is the exact inverse. Start from the "case A" guess
    // (L-1 = RIV/N, RB_start = RIV%N). If L-1 + RB_start exceeds N then the
    // allocation was actually "mirrored" (case B), so undo the mirror:
    //   L        = N - (RIV/N)
    //   RB_start = N - 1 - (RIV%N)
    // and only then add the trailing +1 to get the final length.
    const uint32_t N = n_rb_bwp;
    if (N == 0) { n_start_prb = 0; n_length_prb = 0; return; }

    uint32_t l        = riv / N;          // tentative L-1 (case A)
    uint32_t rb_start = riv % N;          // tentative RB_start (case A)

    if (rb_start + l > N - 1) {           // case B: mirrored
        l        = N - l;
        rb_start = N - 1 - rb_start;
    }

    n_start_prb  = rb_start;
    n_length_prb = l + 1;                 // final length
}

uint32_t riv_encode(uint32_t n_start_prb, uint32_t n_length_prb, uint32_t n_rb_bwp)
{
    // TS 38.214 5.1.2.2.1 (Type-1), exact inverse of riv_decode:
    //   if (L-1) <= floor(N/2) :  RIV = N*(L-1) + RB_start
    //   else                   :  RIV = N*(N-L+1) + (N-1-RB_start)
    const uint32_t N = n_rb_bwp;
    if (N == 0 || n_length_prb == 0 || n_length_prb > N ||
        n_start_prb > N - n_length_prb) {
        return 0;   // invalid / unallocatable
    }
    const uint32_t L = n_length_prb;
    const uint32_t R = n_start_prb;
    if (L - 1 <= N / 2) return N * (L - 1) + R;
    return N * (N - L + 1) + (N - 1 - R);
}

DciFormat10 DciFormat10::parse(const std::vector<uint8_t>& bits, uint32_t n_rb_bwp)
{
    DciFormat10 d;
    if (bits.empty()) return d;

    size_t pos = 0;
    // Reads `n` bits MSB-first from the vector and packs them into a uint.
    auto take = [&](size_t n) -> uint32_t {
        uint32_t v = 0;
        for (size_t i = 0; i < n && pos < bits.size(); ++i) {
            v = (v << 1) | (bits[pos++] & 0x1);
        }
        return v;
    };

    // 1 bit: 0 = UL grant (format 0_0), 1 = DL assignment (format 1_0).
    d.identifier_dci_formats = take(1);
    if (d.identifier_dci_formats != 1) {
        return d;   // not a DL 1_0 schedule -> mark invalid
    }

    // Frequency-domain resource assignment (RIV) of riv_bits.
    const uint32_t riv_bits = static_cast<uint32_t>(std::ceil(std::log2(
        static_cast<double>(n_rb_bwp) * (n_rb_bwp + 1) / 2.0)));
    uint32_t riv = 0;
    for (uint32_t i = 0; i < riv_bits; ++i) {
        riv = (riv << 1) | take(1);
    }
    d.freq_domain_riv = riv;
    riv_decode(riv, n_rb_bwp, d.n_start_prb, d.n_length_prb);

    // Remaining fixed fields, in the ordering of TS 38.212 7.3.1.2.1.
    d.time_domain_assignment = static_cast<uint8_t>(take(4));
    d.vrb_to_prb_mapping     = static_cast<uint8_t>(take(1));
    d.mcs                    = static_cast<uint8_t>(take(5));
    d.new_data_indicator     = static_cast<uint8_t>(take(1));
    d.redundancy_version     = static_cast<uint8_t>(take(2));
    d.harq_process_number    = static_cast<uint8_t>(take(4));
    d.dl_assignment_index    = static_cast<uint8_t>(take(2));
    d.tpc_for_pucch          = static_cast<uint8_t>(take(2));
    d.pucch_resource_indicator = static_cast<uint8_t>(take(3));
    d.pdsch_harq_timing      = static_cast<uint8_t>(take(3));

    d.valid = true;
    return d;
}

} // namespace gone::nr
