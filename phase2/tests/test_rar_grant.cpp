// test_rar_grant.cpp
// ==================
// Portable unit test for the TS 38.213 Table 8.2-1 RAR UL grant decoder
// (gone::nr::decode_rar_ul_grant / rar_k2_slots). No UHD or srsRAN-4G needed:
//
//  1. The 27 grant bits are built field-by-field (hop/ RIV / TDRA / MCS / TPC /
//     CSI) and decoded back — the field order / widths are proven correct.
//  2. The same bits are written into a real MAC RAR subPDU byte stream exactly
//     the way srsRAN_4G's mac_rar_pdu_nr.cc serializes it, then read back with
//     the same bit extraction srsRAN uses (ul_grant[0..2] in octet1, 24 bits in
//     octets 2-4, MSB first). This proves our decoder matches what parse_mac_rar()
//     will hand it over the air.
//  3. K2 mapping across the full default PUSCH TDRA table is checked.
//
// Build (Mac or Latte): in build/phase2, `make test_rar_grant && ./test_rar_grant`.

#include "5gone/mac_rar.hpp"

#include <cstdint>
#include <cstdio>
#include <vector>

using namespace gone::nr;

static int failures = 0;

#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                \
            ++failures;                                                                \
        }                                                                              \
    } while (0)

namespace {

// Pack a 27-bit RAR UL grant MSB-first from its TS 38.213 Tab 8.2-1 fields.
std::vector<uint8_t> pack_grant(bool hop, uint32_t riv14, uint8_t tdra, uint8_t mcs, uint8_t tpc, bool csi)
{
    std::vector<uint8_t> b;
    auto push = [&](uint32_t v, unsigned n) {
        for (unsigned i = n; i-- > 0;) {
            b.push_back(static_cast<uint8_t>((v >> i) & 1u));
        }
    };
    push(hop ? 1u : 0u, 1);
    push(riv14, 14);
    push(tdra, 4);
    push(mcs, 4);
    push(tpc, 3);
    push(csi ? 1u : 0u, 1);
    return b;
}

// Serialize a MAC RAR subPDU to bytes exactly like srsRAN_4G mac_rar_pdu_nr.cc,
// then extract the 27 grant bits back the way its read_subpdu() does.
std::vector<uint8_t> pdu_to_grant_bits(uint8_t rapid,
                                       uint32_t ta,
                                       const std::vector<uint8_t>& ug /* 27 bits */,
                                       uint16_t tc_rnti)
{
    // pdu[0] = E|T|RAPID header, pdu[1..7] = 7-byte MAC RAR body.
    std::vector<uint8_t> pdu(8, 0);
    pdu[0] = rapid & 0x3F;
    pdu[1] = static_cast<uint8_t>((ta >> 5) & 0x7F);                       // TA[11:5]
    pdu[2] = static_cast<uint8_t>(((ta & 0x1F) << 3) | (ug[0] << 2) | (ug[1] << 1) | ug[2]);
    for (unsigned i = 0; i < 3; ++i) {                                     // ug[3..26]
        pdu[3 + i] = 0;
        for (unsigned j = 0; j < 8; ++j) {
            pdu[3 + i] = static_cast<uint8_t>((pdu[3 + i] << 1) | ug[3 + 8 * i + j]);
        }
    }
    pdu[6] = static_cast<uint8_t>(tc_rnti >> 8);                           // TC-RNTI big-endian
    pdu[7] = static_cast<uint8_t>(tc_rnti & 0xFF);

    // Mirrors srsRAN read_subpdu() bit extraction:
    std::vector<uint8_t> b;
    auto oct_msb_first = [&](uint8_t x) {
        for (int j = 7; j >= 0; --j) {
            b.push_back(static_cast<uint8_t>((x >> j) & 1u));
        }
    };
    b.push_back((pdu[2] >> 2) & 1u);
    b.push_back((pdu[2] >> 1) & 1u);
    b.push_back((pdu[2] >> 0) & 1u);
    oct_msb_first(pdu[3]);
    oct_msb_first(pdu[4]);
    oct_msb_first(pdu[5]);
    return b;   // 27 bits, index 0 = hop flag (MSB)
}

void test_field_by_field(const std::vector<uint8_t>& bits, const char* name)
{
    const uint32_t n_prb = 51; // 20 MHz @ 30 kHz lab cell
    const RarUlGrant g   = decode_rar_ul_grant(bits, n_prb);

    CHECK(g.valid);
    CHECK(g.freq_hopping);
    CHECK(g.freq_domain_assignment == 32);                                  // RIV 0x20
    CHECK(g.rb_start == 32);
    CHECK(g.rb_len == 1);                                                   // RIV=32, N=51 -> (rb_start=32, len=1)
    CHECK(g.time_domain_assignment == 6);
    CHECK(g.k2_slots == 2);                                                 // TDRA row 6 -> K2 = 2 slots
    CHECK(g.mcs == 9);
    CHECK(g.tpc_for_pusch == 5);
    CHECK(g.csi_request);
    std::printf("[ok] %s: hop=1 riv=32( rb=%u len=%u) tdra=6 k2=%u mcs=9 tpc=5 csi=1\n",
                name, g.rb_start, g.rb_len, g.k2_slots);
}

} // namespace

int main()
{
    std::printf("test_rar_grant: TS 38.213 Tab 8.2-1 RAR UL grant decoder\n");

    // Case 1: a full 27-bit grant, packed field-by-field.
    {
        const std::vector<uint8_t> bits = pack_grant(true, 32, 6, 9, 5, true);
        CHECK(bits.size() == 27);
        test_field_by_field(bits, "field-by-field");
    }

    // Case 2: same grant, but serialized into a real MAC RAR subPDU and read
    // back with srsRAN_4G's extraction — must decode identically.
    {
        const std::vector<uint8_t> bits = pack_grant(true, 32, 6, 9, 5, true);
        const std::vector<uint8_t> ext  = pdu_to_grant_bits(/*rapid=*/7, /*ta=*/0x123, bits, /*tc_rnti=*/0x1357);
        CHECK(ext == bits);   // byte round-trip must be lossless
        test_field_by_field(ext, "mac-rar-pdu");
    }

    // Case 3: all-zero grant -> all fields zero.
    {
        const std::vector<uint8_t> bits(27, 0);
        const RarUlGrant g = decode_rar_ul_grant(bits, 51);
        CHECK(g.valid);
        CHECK(!g.freq_hopping);
        CHECK(g.freq_domain_assignment == 0);
        CHECK(g.rb_start == 0);
        CHECK(g.rb_len == 1);   // RIV 0 -> single PRB at 0
        CHECK(g.time_domain_assignment == 0);
        CHECK(g.k2_slots == 1); // row 0 -> K2 = 1
        CHECK(g.mcs == 0);
        CHECK(g.tpc_for_pusch == 0);
        CHECK(!g.csi_request);
        std::printf("[ok] all-zero: valid, rb=(%u,%u) k2=%u\n", g.rb_start, g.rb_len, g.k2_slots);
    }

    // Case 4: truncated bit vector is rejected.
    {
        const RarUlGrant g = decode_rar_ul_grant(std::vector<uint8_t>(20, 0), 51);
        CHECK(!g.valid);
        CHECK(!g.freq_hopping);
        std::printf("[ok] truncated (<27) rejected\n");
    }

    // Case 5: full K2 default-table mapping (TS 38.214 Tab 6.1.2.1.1-1).
    {
        static const uint32_t expect[16] = {1, 1, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4, 4, 8, 8};
        for (uint8_t row = 0; row < 16; ++row) {
            if (rar_k2_slots(row) != expect[row]) {
                std::printf("FAIL k2 row %u: got %u want %u\n", row, rar_k2_slots(row), expect[row]);
                ++failures;
            }
        }
        std::printf("[ok] k2 default-table (16 rows)\n");
    }

    if (failures == 0) {
        std::printf("PASS\n");
        return 0;
    }
    std::printf("FAILED (%d)\n", failures);
    return 1;
}