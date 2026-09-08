// // #include "srsran/phy/upper/channel_processors/pdcch_processor.h"
// // // #include "srsran/phy/upper/channel_processors/pdcch/pdcch_dci_packing.h"
// // #include "srsran/ran/pdcch/dci_packing.h"
// // #include "srsran/phy/upper/channel_processors/pdsch/pdsch_processor.h"
// // #include "srsran/fapi_adaptor/mac/messages/pdcch.h"
// // #include "srsran/mac/mac_pdu_format.h"
// // #include "srsran/support/units.h"
// // #include <span>
// // #include <vector>
// // #include <iostream>

// // struct mac_rar_result {
// //   uint16_t timing_advance;
// //   uint32_t ul_grant;
// //   uint16_t temp_c_rnti;
// //   bool     found = false;
// // };

// // // Main processing function linking Steps 1 to 4
// // bool process_msg2_rar(
// //     const std::vector<srsran::log_likelihood_ratio>& pdcch_llrs,
// //     const std::vector<srsran::log_likelihood_ratio>& pdsch_llrs,
// //     uint16_t ra_rnti,
// //     uint8_t target_rapid,
// //     mac_rar_result& out_rar)
// // {
// //   // =========================================================================
// //   // STEP 1: Decode PDCCH (Control Channel)
// //   // =========================================================================
// //   srsran::pdcch_processor pdcch_proc;
// //   srsran::pdcch_processor::cfg_t pdcch_cfg{};
  
// //   pdcch_cfg.rnti = srsran::to_rnti(ra_rnti);
// //   pdcch_cfg.dci_format = srsran::dci_format::FORMAT_1_0;
// //   pdcch_cfg.payload_len = 39; // Fallback DCI Format 1_0 payload length in bits

// //   srsran::bounded_bit_buffer<srsran::units::bits(64)> raw_dci_bits;

// //   bool pdcch_ok = pdcch_proc.decode(
// //       raw_dci_bits,
// //       srsran::span<const srsran::log_likelihood_ratio>(pdcch_llrs),
// //       pdcch_cfg
// //   );

// //   if (!pdcch_ok) {
// //     std::cerr << "[Step 1 Failed] PDCCH polar decoding or RA-RNTI CRC unmasking failed.\n";
// //     return false;
// //   }

// //   // =========================================================================
// //   // STEP 2: Unpack DCI Parameters for PDSCH Demodulation
// //   // =========================================================================
// //   srsran::dci_format_1_0_rar dci_payload{};
// //   srsran::unpack_dci_1_0_rar(dci_payload, raw_dci_bits);

// //   // Configure PDSCH decoding parameters using extracted DCI values
// //   srsran::pdsch_processor::grant_t pdsch_grant{};
// //   pdsch_grant.rnti = srsran::to_rnti(ra_rnti);
// //   pdsch_grant.freq_allocation = dci_payload.freq_alloc;
// //   pdsch_grant.time_allocation = dci_payload.time_alloc;
// //   pdsch_grant.mcs = dci_payload.mcs;
// //   pdsch_grant.rv = 0; // Standard initial transmission RV for RAR

// //   // =========================================================================
// //   // STEP 3: Decode PDSCH to Extract Raw Transport Block Bytes
// //   // =========================================================================
// //   srsran::pdsch_processor pdsch_proc;
// //   std::vector<uint8_t> mac_tb_bytes(128); // Buffer for raw MAC Transport Block

// //   srsran::pdsch_processor::decoding_result pdsch_result = pdsch_proc.decode(
// //       srsran::span<uint8_t>(mac_tb_bytes),
// //       srsran::span<const srsran::log_likelihood_ratio>(pdsch_llrs),
// //       pdsch_grant
// //   );

// //   if (!pdsch_result.tb_crc_ok) {
// //     std::cerr << "[Step 3 Failed] PDSCH LDPC decoding or Transport Block CRC failed.\n";
// //     return false;
// //   }

// //   // Resizing buffer to actual decoded Transport Block size
// //   mac_tb_bytes.resize(pdsch_result.tb_len_bytes);

// //   // =========================================================================
// //   // STEP 4: Parse MAC Subheaders & Extract Target RAR Payload
// //   // =========================================================================
// //   size_t offset = 0;
// //   while (offset < mac_tb_bytes.size()) {
// //     uint8_t header_byte = mac_tb_bytes[offset];
// //     bool extension = (header_byte & 0x80) != 0;
// //     bool type_bit  = (header_byte & 0x40) != 0; // 1 = RAPID header, 0 = BI header

// //     if (!type_bit) {
// //       // Backoff Indicator (BI) subheader (1 byte)
// //       offset += 1;
// //     } else {
// //       // RAPID subheader (1 byte)
// //       uint8_t rapid = header_byte & 0x3F;
// //       offset += 1;

// //       if (rapid == target_rapid) {
// //         // Target RAPID found -> extract 7-byte MAC RAR body
// //         if (offset + 7 > mac_tb_bytes.size()) {
// //           std::cerr << "[Step 4 Failed] Corrupted RAR payload length.\n";
// //           return false;
// //         }

// //         const uint8_t* rar_ptr = &mac_tb_bytes[offset];

// //         // 11-bit Timing Advance
// //         out_rar.timing_advance = ((uint16_t)(rar_ptr[0] & 0x7F) << 4) | ((rar_ptr[1] & 0xF0) >> 4);
        
// //         // 27-bit Uplink Grant for Msg3
// //         out_rar.ul_grant = ((uint32_t)(rar_ptr[1] & 0x0F) << 23) |
// //                            ((uint32_t)(rar_ptr[2]) << 15) |
// //                            ((uint32_t)(rar_ptr[3]) << 7) |
// //                            ((uint32_t)(rar_ptr[4] & 0xFE) >> 1);

// //         // 16-bit Temporary C-RNTI
// //         out_rar.temp_c_rnti = ((uint16_t)rar_ptr[5] << 8) | rar_ptr[6];
// //         out_rar.found = true;
// //         return true;
// //       }

// //       // Skip this non-matching RAR payload (7 bytes)
// //       offset += 7;
// //     }

// //     if (!extension) break; // Last subheader reached
// //   }

// //   std::cerr << "[Step 4 Failed] Target RAPID " << (int)target_rapid << " not found in MAC PDU.\n";
// //   return false;
// // }

// #include "srsran/phy/upper/channel_processors/pdcch_processor.h"
// #include "srsran/phy/upper/channel_processors/pdsch/pdsch_processor.h"
// #include "srsran/ran/pdcch/dci_packing.h"
// #include "srsran/mac/mac_pdu_format.h"
// #include "srsran/support/units.h"
// #include <span>
// #include <vector>
// #include <iostream>

// struct mac_rar_result {
//   uint16_t timing_advance;
//   uint32_t ul_grant;
//   uint16_t temp_c_rnti;
//   bool     found = false;
// };

// // Main processing pipeline connecting Steps 1 through 4
// bool process_msg2_rar(
//     const std::vector<srsran::log_likelihood_ratio>& pdcch_llrs,
//     const std::vector<srsran::log_likelihood_ratio>& pdsch_llrs,
//     uint16_t ra_rnti,
//     uint8_t target_rapid,
//     mac_rar_result& out_rar)
// {
//   // =========================================================================
//   // STEP 1: Decode PDCCH (Control Channel)
//   // =========================================================================
//   srsran::pdcch_processor pdcch_proc;
//   srsran::pdcch_processor::cfg_t pdcch_cfg{};
  
//   pdcch_cfg.rnti = srsran::to_rnti(ra_rnti);
//   pdcch_cfg.dci_format = srsran::dci_format::FORMAT_1_0;
//   pdcch_cfg.payload_len = 39; // Fallback DCI Format 1_0 payload length in bits

//   srsran::bounded_bit_buffer<srsran::units::bits(64)> raw_dci_bits;

//   bool pdcch_ok = pdcch_proc.decode(
//       raw_dci_bits,
//       srsran::span<const srsran::log_likelihood_ratio>(pdcch_llrs),
//       pdcch_cfg
//   );

//   if (!pdcch_ok) {
//     std::cerr << "[Step 1 Failed] PDCCH polar decoding or RA-RNTI CRC unmasking failed.\n";
//     return false;
//   }

//   // =========================================================================
//   // STEP 2: Unpack DCI Parameters for PDSCH Demodulation
//   // =========================================================================
//   srsran::dci_format_1_0_rar dci_payload{};
  
//   // Unpack raw bits into DCI Format 1_0 structure using srsran/ran/pdcch/dci_packing.h
//   bool dci_unpack_ok = srsran::unpack_dci_1_0_rar(dci_payload, raw_dci_bits);
//   if (!dci_unpack_ok) {
//     std::cerr << "[Step 2 Failed] Could not unpack DCI Format 1_0 RAR payload.\n";
//     return false;
//   }

//   // Map DCI payload to PDSCH allocation grant
//   srsran::pdsch_processor::grant_t pdsch_grant{};
//   pdsch_grant.rnti = srsran::to_rnti(ra_rnti);
//   pdsch_grant.freq_allocation = dci_payload.freq_alloc;
//   pdsch_grant.time_allocation = dci_payload.time_alloc;
//   pdsch_grant.mcs = dci_payload.mcs;
//   pdsch_grant.rv = 0; // Standard RV 0 for initial RAR transmission

//   // =========================================================================
//   // STEP 3: Decode PDSCH to Extract Raw Transport Block Bytes
//   // =========================================================================
//   srsran::pdsch_processor pdsch_proc;
//   std::vector<uint8_t> mac_tb_bytes(128); // Storage for raw MAC Transport Block

//   srsran::pdsch_processor::decoding_result pdsch_result = pdsch_proc.decode(
//       srsran::span<uint8_t>(mac_tb_bytes),
//       srsran::span<const srsran::log_likelihood_ratio>(pdsch_llrs),
//       pdsch_grant
//   );

//   if (!pdsch_result.tb_crc_ok) {
//     std::cerr << "[Step 3 Failed] PDSCH LDPC decoding or Transport Block CRC failed.\n";
//     return false;
//   }

//   mac_tb_bytes.resize(pdsch_result.tb_len_bytes);

//   // =========================================================================
//   // STEP 4: Parse MAC Subheaders & Extract Target RAR Payload
//   // =========================================================================
//   size_t offset = 0;
//   while (offset < mac_tb_bytes.size()) {
//     uint8_t header_byte = mac_tb_bytes[offset];
//     bool extension = (header_byte & 0x80) != 0;
//     bool type_bit  = (header_byte & 0x40) != 0; // 1 = RAPID header, 0 = BI header

//     if (!type_bit) {
//       // Backoff Indicator (BI) subheader (1 byte)
//       offset += 1;
//     } else {
//       // RAPID subheader (1 byte)
//       uint8_t rapid = header_byte & 0x3F;
//       offset += 1;

//       if (rapid == target_rapid) {
//         // Match found -> parse 7-byte MAC RAR payload
//         if (offset + 7 > mac_tb_bytes.size()) {
//           std::cerr << "[Step 4 Failed] Corrupted RAR payload length.\n";
//           return false;
//         }

//         const uint8_t* rar_ptr = &mac_tb_bytes[offset];

//         // Extract 11-bit Timing Advance
//         out_rar.timing_advance = ((uint16_t)(rar_ptr[0] & 0x7F) << 4) | ((rar_ptr[1] & 0xF0) >> 4);
        
//         // Extract 27-bit Uplink Grant
//         out_rar.ul_grant = ((uint32_t)(rar_ptr[1] & 0x0F) << 23) |
//                            ((uint32_t)(rar_ptr[2]) << 15) |
//                            ((uint32_t)(rar_ptr[3]) << 7) |
//                            ((uint32_t)(rar_ptr[4] & 0xFE) >> 1);

//         // Extract 16-bit Temporary C-RNTI
//         out_rar.temp_c_rnti = ((uint16_t)rar_ptr[5] << 8) | rar_ptr[6];
//         out_rar.found = true;
//         return true;
//       }

//       // Skip non-matching 7-byte RAR body
//       offset += 7;
//     }

//     if (!extension) break; // End of headers
//   }

//   std::cerr << "[Step 4 Failed] RAPID " << (int)target_rapid << " not found in MAC PDU.\n";
//   return false;
// }