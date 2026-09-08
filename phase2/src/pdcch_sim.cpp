// idk what is this for


#include <iostream>
#include <vector>
#include <cstdint>
#include <random>
#include <bitset>
#include <algorithm>

// --- Data Structures ---
struct DCIFormat1_0_RAR {
    uint32_t frequencyAllocation = 0; // Raw frequency domain bitmask
    uint8_t timeAllocation = 0;       // k0 offset (slots)
    uint8_t mcs = 0;                  // MCS index
    uint16_t crc = 0;                 // Scrambled CRC
};

struct SearchSpaceCandidate {
    uint16_t cceIndex;
    uint8_t aggregationLevel;
    std::vector<uint8_t> payloadBits; // Contains data + masked CRC
};

// --- Helper Functions ---
// Simplified 3GPP CRC-16 (Polynomial: 0x1021)
uint16_t calculateCRC16(const std::vector<uint8_t>& data) {
    uint16_t crc = 0xFFFF;
    for (uint8_t byte : data) {
        crc ^= (static_cast<uint16_t>(byte) << 8);
        for (int i = 0; i < 8; ++i) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

// --- Simulation Engine Class ---
class PdcchRarSimulator {
private:
    uint16_t raRnti;
    // std::mt19935 rng;

public:
    PdcchRarSimulator() {
        srand(time(nullptr));
    }

    // Calculate RA-RNTI (3GPP TS 38.321)
    void initRaRnti(uint8_t s_id, uint8_t t_id, uint8_t f_id, uint8_t ul_carrier_id) {
        raRnti = 1 + s_id + (14 * t_id) + (14 * 80 * f_id) + (14 * 80 * 8 * ul_carrier_id);
        std::cout << "[SIM-TX] Initialized target RA-RNTI: 0x" << std::hex << raRnti << std::dec << "\n";
    }

    // Generator: Creates a valid, scrambled DCI transmission block
    std::vector<uint8_t> generateRarDciBlock(const DCIFormat1_0_RAR& sourceDci) {
        std::vector<uint8_t> block;
        
        // Pack bitfields into 3 raw bytes (Simulated DCI structure)
        uint32_t packedPayload = 0;
        packedPayload |= (sourceDci.frequencyAllocation & 0x3FFF); // 14 bits
        packedPayload |= ((sourceDci.timeAllocation & 0xF) << 14);  // 4 bits
        packedPayload |= ((sourceDci.mcs & 0x1F) << 18);           // 5 bits

        block.push_back((packedPayload >> 16) & 0xFF);
        block.push_back((packedPayload >> 8) & 0xFF);
        block.push_back(packedPayload & 0xFF);

        // Compute pure CRC
        uint16_t pureCrc = calculateCRC16(block);

        // Mask CRC using the calculated RA-RNTI (XOR operation)
        uint16_t maskedCrc = pureCrc ^ raRnti;

        // Append masked CRC to the transmission stream (Big Endian)
        block.push_back((maskedCrc >> 8) & 0xFF);
        block.push_back(maskedCrc & 0xFF);

        std::cout << "[SIM-TX] Generated Payload CRC: 0x" << std::hex << pureCrc 
                  << " -> Scrambled CRC: 0x" << maskedCrc << std::dec << "\n";
        return block;
    }

    // Channel Simulator: Creates noise or dummy candidate lists in the Common Search Space
    std::vector<SearchSpaceCandidate> generateSearchSpace(const std::vector<uint8_t>& realDciBlock) {
        std::vector<SearchSpaceCandidate> searchSpace;
        std::uniform_int_distribution<int> byteDist(0, 255);

        // Insert dummy noise candidates to simulate a true receiver "blind decode" scenario
        for (int i = 0; i < 3; ++i) {
            SearchSpaceCandidate dummy;
            dummy.cceIndex = i * 4;
            dummy.aggregationLevel = 4;
            for (size_t j = 0; j < realDciBlock.size(); ++j) {
                dummy.payloadBits.push_back(rand() % 256);
            }
            searchSpace.push_back(dummy);
        }

        // Insert the valid candidate inside a random slot position
        SearchSpaceCandidate validCandidate;
        validCandidate.cceIndex = 12;
        validCandidate.aggregationLevel = 4;
        validCandidate.payloadBits = realDciBlock;
        searchSpace.push_back(validCandidate);

        // Shuffle to simulate actual radio pipeline parsing randomness
        std::random_shuffle(searchSpace.begin(), searchSpace.end()); 
        return searchSpace;
    }

    // Decoder: Processes candidates and runs real-time descrambling validation
    bool runBlindDecodeLoop(const std::vector<SearchSpaceCandidate>& candidates, DCIFormat1_0_RAR& outputDci) {
        std::cout << "\n[RX-DECODE] Starting blind decoding over " << candidates.size() << " CCE candidates...\n";

        for (size_t idx = 0; idx < candidates.size(); ++idx) {
            const auto& cand = candidates[idx];
            
            // Separate data content from appended CRC
            std::vector<uint8_t> dataBits(cand.payloadBits.begin(), cand.payloadBits.end() - 2);
            uint16_t rxMaskedCrc = (cand.payloadBits[cand.payloadBits.size() - 2] << 8) | 
                                    cand.payloadBits[cand.payloadBits.size() - 1];

            // Recompute local verification CRC
            uint16_t localCrc = calculateCRC16(dataBits);

            // De-scramble the received CRC by applying RA-RNTI mask
            uint16_t unmaskedCrc = rxMaskedCrc ^ raRnti;

            std::cout << " -> Processing Candidate #" << idx << " (CCE Index: " << cand.cceIndex << ")\n"
                      << "    Local CRC: 0x" << std::hex << localCrc 
                      << " | Unmasked Rx CRC: 0x" << unmaskedCrc << std::dec << "\n";

            // Verify if the checksum passes check parameters
            if (localCrc == unmaskedCrc) {
                std::cout << " [!] Match Detected! Descrambling successful for RA-RNTI.\n";

                // Unpack payload bitfields
                uint32_t packedPayload = (dataBits[0] << 16) | (dataBits[1] << 8) | dataBits[2];
                outputDci.frequencyAllocation = packedPayload & 0x3FFF;
                outputDci.timeAllocation = (packedPayload >> 14) & 0xF;
                outputDci.mcs = (packedPayload >> 18) & 0x1F;
                outputDci.crc = rxMaskedCrc;

                return true; 
            }
        }
        return false;
    }
};

// --- Execution Entrypoint ---
int main() {
    PdcchRarSimulator sim;

    // 1. Configure PRACH attributes to formulate transmission window parameters
    uint8_t symbol_id = 4;
    uint8_t slot_id = 2;
    uint8_t freq_id = 0;
    uint8_t ul_carrier = 0;
    sim.initRaRnti(symbol_id, slot_id, freq_id, ul_carrier);

    // 2. Define target RAR configuration markers (Tx Side)
    DCIFormat1_0_RAR networkConfig;
    networkConfig.frequencyAllocation = 0x1A25; // Target PRBs allocated
    networkConfig.timeAllocation = 3;           // PDSCH Msg2 scheduled 3 slots away (k0)
    networkConfig.mcs = 8;                      // MCS schema assignment

    // 3. Generate baseline scrambled data blocks
    std::vector<uint8_t> airwaves = sim.generateRarDciBlock(networkConfig);

    // 4. Create simulation search array representing noise + targets
    std::vector<SearchSpaceCandidate> currentSearchSpace = sim.generateSearchSpace(airwaves);

    // 5. Fire blind processing loop
    DCIFormat1_0_RAR decodedResult;
    bool status = sim.runBlindDecodeLoop(currentSearchSpace, decodedResult);

    // 6. Output analysis results
    if (status) {
        std::cout << "\n============================================\n"
                  << "  SIMULATION RESULT: SUCCESS                  \n"
                  << "============================================\n"
                  << " Decoded Frequency PRB Mask: 0x" << std::hex << decodedResult.frequencyAllocation << std::dec << "\n"
                  << " Scheduled Slot Delay (k0) : " << (int)decodedResult.timeAllocation << " slots\n"
                  << " Decoded Target MCS Level  : MCS " << (int)decodedResult.mcs << "\n"
                  << " Raw Received Masked CRC   : 0x" << std::hex << decodedResult.crc << std::dec << "\n";
    } else {
        std::cout << "\n[SIM-ERROR] Simulation failed. Target block dropped.\n";
    }

    return 0;
}
