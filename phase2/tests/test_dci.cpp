// Step 6 test: nr_dci (DCI 1_0 parse + RIV decode), nr_coreset.
//
//   g++ -std=c++17 -I ../include test_dci.cpp ../src/nr_dci.cpp -o test_dci && ./test_dci

#include "5gone/nr_dci.hpp"
#include "5gone/nr_coreset.hpp"

#include <cstdio>
#include <vector>

using namespace gone::nr;

static int g_fail = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); ++g_fail; } \
} while (0)

int main()
{
    // RIV decode: N=51, start=5, len=3 -> RIV = 51*(3-1)+5 = 107
    {
        uint32_t s = 0, l = 0;
        riv_decode(107, 51, s, l);
        printf("riv_decode(107,51): start=%u len=%u (expect 5 3)\n", s, l);
        CHECK(s == 5 && l == 3, "RIV decode basic");
    }
    // Mirror case: choose len > N/2, e.g. start=0, len=40 (N=51).
    // (L-1)=39 > 25 -> RIV = N*(N-L+1)+(N-1-RB_start) = 51*12+50 = 662
    {
        uint32_t s = 0, l = 0;
        riv_decode(662, 51, s, l);
        printf("riv_decode(662,51): start=%u len=%u (expect 0 40)\n", s, l);
        CHECK(s == 0 && l == 40, "RIV decode mirror");
    }
    // Full BWP: start=0, len=51 -> (L-1)=50>25 -> RIV=51*(1)+(50-0)=101
    {
        uint32_t s = 0, l = 0;
        riv_decode(101, 51, s, l);
        printf("riv_decode(101,51): start=%u len=%u (expect 0 51)\n", s, l);
        CHECK(s == 0 && l == 51, "RIV decode full BWP");
    }

    // DCI 1_0 size for 51-RB BWP should be 39 bits.
    const uint32_t sz = dci_format10_bits(51);
    printf("dci_format10_bits(51)=%u (expect 39)\n", sz);
    CHECK(sz == 39, "DCI 1_0 is 39 bits for 51 RB");

    // Build a synthetic DCI 1_0 bit vector (MSB first) and parse it back.
    std::vector<uint8_t> b(39, 0);
    size_t pos = 0;
    auto put = [&](uint32_t v, int n){
        for (int i = n - 1; i >= 0; --i) b[pos++] = (v >> i) & 1;
    };
    put(1, 1);            // identifier = 1 (DL 1_0)
    uint32_t riv = 107;   // start=5 len=3
    for (int i = 10; i >= 0; --i) b[pos++] = (riv >> i) & 1;  // 11-bit RIV
    put(3, 4);            // time-domain assignment = 3
    put(0, 1);            // vrb_to_prb = 0
    put(9, 5);            // MCS = 9
    put(1, 1);            // NDI = 1
    put(2, 2);            // RV = 2
    put(6, 4);            // HARQ proc = 6
    put(1, 2);            // DAI = 1
    put(0, 2);            // TPC = 0
    put(4, 3);            // PUCCH resource = 4
    put(7, 3);            // PDSCH HARQ timing = 7

    DciFormat10 d = DciFormat10::parse(b, 51);
    printf("valid=%d identifier=%u riv=%u start=%u len=%u tda=%u mcs=%u harq=%u pri=%u timing=%u\n",
        d.valid, d.identifier_dci_formats, d.freq_domain_riv,
        d.n_start_prb, d.n_length_prb, d.time_domain_assignment,
        d.mcs, d.harq_process_number, d.pucch_resource_indicator, d.pdsch_harq_timing);
    CHECK(d.valid, "DCI parsed as valid 1_0");
    CHECK(d.identifier_dci_formats == 1, "identifier = 1");
    CHECK(d.freq_domain_riv == 107 && d.n_start_prb == 5 && d.n_length_prb == 3, "RIV field round-trip");
    CHECK(d.time_domain_assignment == 3, "TDA=3");
    CHECK(d.mcs == 9, "MCS=9");
    CHECK(d.harq_process_number == 6, "HARQ=6");
    CHECK(d.pucch_resource_indicator == 4, "PUCCH=4");
    CHECK(d.pdsch_harq_timing == 7, "timing=7");

    printf("done: %d failure(s)\n", g_fail);
    return g_fail ? 1 : 0;
}