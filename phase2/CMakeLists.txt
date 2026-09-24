cmake_minimum_required(VERSION 3.16)
project(5gone_attacker VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(UHD REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

if(NOT TARGET UHD::UHD)
  add_library(UHD::UHD INTERFACE IMPORTED)
  set_target_properties(UHD::UHD PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${UHD_INCLUDE_DIRS}"
    INTERFACE_LINK_LIBRARIES "${UHD_LIBRARIES}"
  )
endif()

option(5GONE_ENABLE_SRSRAN "Link srsRAN Project PHY libs if available" ON)
# Old srsRAN 4G C API (srsRANRF) used by the PDCCH polar/CRC decoder
# (nr_pdcch_decode_srsran.ipp). Enable only when built on the Linux box.
option(5GONE_ENABLE_SRSRAN_OLD "Link srsRAN 4G (old C API) for full polar decode" ON)

set(5GONE_SOURCES
  src/config.cpp
  src/empty_mac_pdu.cpp
  src/pusch_encoder.cpp
  src/radio_uhd.cpp
  src/tdd_gate.cpp
  src/cell_sync.cpp
  src/sample_clock.cpp
  src/ul_gate.cpp
  src/nr_prach.cpp
  src/rar_monitor.cpp
  src/attack_engine.cpp
  src/main.cpp
  # Ported 5GSniffer DL PDCCH/RAR decode chain
  src/nr_pn.cpp
  src/nr_dsp.cpp
  src/nr_dmrs.cpp
  src/nr_symbol.cpp
  src/nr_ofdm.cpp
  src/nr_dci.cpp
  src/nr_pdcch.cpp
  src/nr_rar_decoder.cpp
  # PDSCH DL-SCH decode + MAC RAR parse (added in Step 5)
  src/nr_pdsch.cpp
  src/mac_rar.cpp
  # NR cell search: PSS/SSS generation + detection (Step 1)
  src/nr_pss.cpp
  # Loopback timing harness: TX->RX latency measurement (Step 4)
  src/latency.cpp
  # Synthetic RAR-slot builder for the software-victim collision test (Step 4b)
  src/nr_rar_tx.cpp
)

add_executable(5gone-rar-dos ${5GONE_SOURCES})
target_include_directories(5gone-rar-dos PRIVATE include ${UHD_INCLUDE_DIRS})

# liquid-dsp speed-up for the OFDM FFT (nr_ofdm.cpp). When present we define
# HAVE_LIQUID and link libliquid; otherwise nr_ofdm falls back to a scalar FFT.
find_library(LIQUID_LIBRARY NAMES liquid)
if(LIQUID_LIBRARY)
  find_path(LIQUID_INCLUDE_DIR NAMES liquid/liquid.h)
  if(LIQUID_INCLUDE_DIR)
    target_compile_definitions(5gone-rar-dos PRIVATE HAVE_LIQUID=1)
    target_include_directories(5gone-rar-dos PRIVATE ${LIQUID_INCLUDE_DIR})
    target_link_libraries(5gone-rar-dos PRIVATE ${LIQUID_LIBRARY})
    message(STATUS "5Gone: liquid-dsp FFT enabled (${LIQUID_INCLUDE_DIR})")
  endif()
endif()

# --- PSS/SSS cell-search test (Step 1) ---
# Self-contained: no UHD, no srsRAN. Verifies sequence generation and
# PSS/SSS detection both in the frequency domain (on a demodulated symbol)
# and in the time domain (matched-filter timing peak on the raw IQ stream).
set(PSS_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
)
add_executable(test_pss ${PSS_TEST_SRC})
target_include_directories(test_pss PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_pss PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_pss PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_pss PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_pss PRIVATE m)

# --- CellSync test (Step 2) ---
# Self-contained: no UHD, no srsRAN. Synthesizes a lab SSB slot (with a
# lead-in and a CFO offset), locks CellSync onto it, checks the slot-clock
# math, and verifies noise / wrong-PCI buffers do NOT lock.
set(CELLSYNC_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
)
add_executable(test_cell_sync ${CELLSYNC_TEST_SRC})
target_include_directories(test_cell_sync PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_cell_sync PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_cell_sync PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_cell_sync PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_cell_sync PRIVATE m)

# --- UL-gate test (Step 4) ---
# Pure math, no UHD: locks CellSync on a synthetic SSB slot, then checks that a
# RAR at a known absolute slot maps to the correct Msg3 UL slot window, rejects
# DL-scheduled and pre-lock windows, and nails the advance-subtracted TX time.
set(ULGATE_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_ul_gate.cpp"
  "${CMAKE_SOURCE_DIR}/src/cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/tdd_gate.cpp"
  "${CMAKE_SOURCE_DIR}/src/sample_clock.cpp"
  "${CMAKE_SOURCE_DIR}/src/ul_gate.cpp"
)
add_executable(test_ul_gate ${ULGATE_TEST_SRC})
target_include_directories(test_ul_gate PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_ul_gate PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_ul_gate PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_ul_gate PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_ul_gate PRIVATE m)

# --- Loopback latency harness test (Step 4) ---
# Self-contained: no UHD, no srsRAN. Injects the marker burst into a synthetic
# RX buffer at a known offset, then checks measure_latency() recovers exactly
# that offset (and rejects windows that exclude it, and pure noise).
set(LOOPBACK_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_loopback.cpp"
  "${CMAKE_SOURCE_DIR}/src/latency.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
)
add_executable(test_loopback ${LOOPBACK_TEST_SRC})
target_include_directories(test_loopback PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_loopback PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_loopback PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_loopback PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_loopback PRIVATE m)

# --- Collision test (Step 4b) ---
# Self-contained: no UHD, no srsRAN. Builds the synthetic RAR slot + a louder
# conflicting copy, injects them into synthetic RX at known offsets, and checks
# the same victim logic run_collide() uses sees one grant in control and a
# duplicate when the attack overlaps the slot.
set(COLLIDE_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_collide.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_tx.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/latency.cpp"
)
add_executable(test_collide ${COLLIDE_TEST_SRC})
target_include_directories(test_collide PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_collide PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_collide PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_collide PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_collide PRIVATE m)

# --- CORESET config-sweep test (R1 blind-spot proof) ---
# Self-contained: no UHD, no srsRAN. Synthesizes a RAR slot whose PDCCH DM-RS
# uses a real gNB-like CORESET (48 PRB @ PRB 2, interleaved, shift=PCI%3),
# proves the default full-BWP CORESET misses it, and that the (config x
# slot-offset) sweep the tool runs recovers it (corr ~1.0). Exercises the
# start_prb offset, interleaved mapping and duration>1 slicing additions.
set(CORESET_SWEEP_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_coreset_sweep.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_tx.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
)
add_executable(test_coreset_sweep ${CORESET_SWEEP_TEST_SRC})
target_include_directories(test_coreset_sweep PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_coreset_sweep PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_coreset_sweep PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_coreset_sweep PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_coreset_sweep PRIVATE m)

# --- Slot-aware window scan test (portable, no UHD/srsRAN) ---
# P2-1: RarMonitor::scan_window slices stream buffers at slot boundaries
# (explicit abs/frame clock), scans the empirical CORESET mini-grid, and
# tracks SIB periodicity buckets. Synthetic dur-2 RAR at abs slots {2,42,9}.
set(SLOT_SCAN_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_slot_scan.cpp"
  "${CMAKE_SOURCE_DIR}/src/rar_monitor.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_tx.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
)
add_executable(test_slot_scan ${SLOT_SCAN_TEST_SRC})
target_include_directories(test_slot_scan PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_slot_scan PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_slot_scan PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_slot_scan PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_slot_scan PRIVATE m)

# --- Null-calibration regression test (portable, no UHD/srsRAN) ---
# R1/P2 defect: "empty" captures scored as high as a real PDCCH because the
# CORESET-sweep and RarMonitor-grid accept fiddles used ABSOLUTE correlation
# thresholds that max-of-extreme statistics on pure noise can reach, then
# decoded zero DCI. The fix measures each grid's own null floor on a synthetic
# noise window and requires the winner to clear it by a margin. Pins:
#   noise sweep -> not accepted, null floor reported;
#   RAR sweep   -> accepted (corr ~1.0) and DCI observations present;
#   monitor grid-> never fires / fills a SIB bucket on noise, fires on the RAR.
set(NULLCAL_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_null_calibration.cpp"
  "${CMAKE_SOURCE_DIR}/tools/decode_capture.hpp"
  "${CMAKE_SOURCE_DIR}/src/cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/rar_monitor.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_tx.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp")
add_executable(test_null_calibration ${NULLCAL_TEST_SRC})
target_include_directories(test_null_calibration PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(test_null_calibration PRIVATE HAVE_LIQUID=1)
  target_include_directories(test_null_calibration PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(test_null_calibration PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(test_null_calibration PRIVATE m)
# Pure TS 38.213 Tab 8.2-1 RAR-grant bit decoding (live-mode Step 1). Links
# only mac_rar.cpp + nr_dci.cpp (riv_decode), so it builds on the Mac workspace
# too — on the Latte it additionally documents parity with the srsRAN-4G MAC
# RAR extraction (parse_mac_rar feeds the same 27-bit array).
add_executable(test_rar_grant
  "${CMAKE_SOURCE_DIR}/tests/test_rar_grant.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp")
target_include_directories(test_rar_grant PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(test_rar_grant PRIVATE m)

# --- PRACH (Msg1) generator unit test (portable, no UHD/srsRAN) ---
# Step 5: B4 short-preamble (139/30k) ZC synthesis, Ncs table, burst geometry, occasion
# math, frequency placement and projection back onto the PRACH subcarriers.
set(PRACH_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_prach.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_prach.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp")
add_executable(test_prach ${PRACH_TEST_SRC})
target_include_directories(test_prach PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(test_prach PRIVATE m)

# --- Rx sample-clock unit test (portable, no UHD) ---
# Live-mode Step 2: UHD time-stamp -> global receive-sample index conversion
# (sample_clock.cpp). Pure math, builds on Mac and Latte.
add_executable(test_sample_clock
  "${CMAKE_SOURCE_DIR}/tests/test_sample_clock.cpp"
  "${CMAKE_SOURCE_DIR}/src/sample_clock.cpp")
target_include_directories(test_sample_clock PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(test_sample_clock PRIVATE m)

# --- Old srsRAN 4G (srsRANRF) for the full PDCCH polar/CRC decode path ---
# Gated behind GONE_HAVE_SRSRAN_OLD. When the old srsRAN_4G C API is available
# (install-srsran-4g.sh) we define the macro, add its headers, and link its
# static libs so nr_pdcch_decode_srsran.ipp compiles in. Without it, the
# decoder runs DM-RS correlation only (still finds WHERE the RAR is, no bits).
set(SRSRAN4G_PREFIX  "$ENV{SRSRAN4G_PREFIX}"  CACHE PATH "srsRAN 4G install prefix")
set(SRSRAN4G_BUILD   "$ENV{SRSRAN4G_BUILD}"   CACHE PATH "srsRAN 4G build tree")
if(5GONE_ENABLE_SRSRAN_OLD)
  if(NOT SRSRAN4G_PREFIX)
    set(SRSRAN4G_PREFIX "${CMAKE_SOURCE_DIR}/../build/srsran4g-install")
  endif()
  if(NOT SRSRAN4G_BUILD)
    set(SRSRAN4G_BUILD "${CMAKE_SOURCE_DIR}/../build/srsran4g")
  endif()

  # Headers: installed under the prefix or live in the build tree.
  set(SRSRAN4G_INCLUDE_DIR "${SRSRAN4G_PREFIX}/include")
  if(NOT EXISTS "${SRSRAN4G_INCLUDE_DIR}/srsran/srsran.h")
    set(SRSRAN4G_INCLUDE_DIR "${SRSRAN4G_BUILD}/lib/include")
  endif()

  if(EXISTS "${SRSRAN4G_INCLUDE_DIR}/srsran/srsran.h")
    # Link dirs: installed prefix lib/ + build-tree subdirs where static libs
    # like support/srslog/asn1 land (not installed by `make install`).
    set(SRSRAN4G_LINK_DIRS
        "${SRSRAN4G_PREFIX}/lib"
        "${SRSRAN4G_BUILD}/lib/src/common"
        "${SRSRAN4G_BUILD}/lib/src/support"
        "${SRSRAN4G_BUILD}/lib/src/srslog"
        "${SRSRAN4G_BUILD}/lib/src/asn1"
        "${SRSRAN4G_BUILD}/lib/src/phy"
        "${SRSRAN4G_BUILD}/lib/src/mac")

    target_compile_definitions(5gone-rar-dos PRIVATE GONE_HAVE_SRSRAN_OLD=1)
    target_include_directories(5gone-rar-dos PRIVATE ${SRSRAN4G_INCLUDE_DIR})
    foreach(dir ${SRSRAN4G_LINK_DIRS})
      target_link_directories(5gone-rar-dos PRIVATE "${dir}")
    endforeach()

    # The polar chanalloc/interleaver functions live in the srsRAN RF submodule
    # (lib/src/phy/fec/polar/). The installed libsran_phy.a may be stale and miss
    # them, so compile the two tiny self-contained .c files DIRECTLY INTO the
    # executable as object sources (avoids static-archive extraction issues) to
    # guarantee srsran_polar_chanalloc_rx / srsran_polar_interleaver_run exist.
    set(SRSRAN4G_SRC "${CMAKE_SOURCE_DIR}/../5GSniffer/5gsniffer/lib/srsRANRF")
    if(EXISTS "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c")
      target_sources(5gone-rar-dos PRIVATE
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
      message(STATUS "5Gone: compiling polar chanalloc/interleaver from srsRAN source ${SRSRAN4G_SRC}")
    else()
      message(WARNING "5Gone: srsRAN polar sources not found under ${SRSRAN4G_SRC}")
    endif()

    # srsran_phy needs fftw3f; srsran_common needs mbedtls. Link the archives
    # FIRST, then their DSO dependencies AFTER: Ubuntu links with --as-needed,
    # so a .so listed BEFORE the static archive that needs it gets dropped and
    # ld fails with "DSO missing from command line" (e.g. dft_fftw.c.o needs
    # fftwf_* once the PDSCH decode pulls that object in).
    find_library(SRSRAN4G_FFTW NAMES fftw3f)
    find_library(SRSRAN4G_MBEDTLS NAMES mbedtls)
    target_link_libraries(5gone-rar-dos PRIVATE
      -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
    if(SRSRAN4G_FFTW)
      target_link_libraries(5gone-rar-dos PRIVATE ${SRSRAN4G_FFTW})
    endif()
    if(SRSRAN4G_MBEDTLS)
      target_link_libraries(5gone-rar-dos PRIVATE ${SRSRAN4G_MBEDTLS})
    endif()
    target_link_libraries(5gone-rar-dos PRIVATE m pthread)

    # ---- Decode-path smoke test (Step 10 confirmation) ----
    # test_rar.cpp builds the same RarDecoder exactly as live mode does (23.04MHz,
    # 30kHz, PCI 1, 51 PRB), feeds pure noise through the full chain, and prints
    # the [rar-dl] init line. Link it against exactly the srsRAN 4G libs/objects
    # the main binary uses so GONE_HAVE_SRSRAN_OLD polar decode is active.
    set(SRSRAN4G_TEST_SRC
      "${CMAKE_SOURCE_DIR}/tests/test_rar.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
      "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
    )
    add_executable(test_rar ${SRSRAN4G_TEST_SRC})
    target_compile_definitions(test_rar PRIVATE GONE_HAVE_SRSRAN_OLD=1)
    target_include_directories(test_rar PRIVATE
      "${CMAKE_SOURCE_DIR}/include"
      ${SRSRAN4G_INCLUDE_DIR})
    foreach(dir ${SRSRAN4G_LINK_DIRS})
      target_link_directories(test_rar PRIVATE "${dir}")
    endforeach()
    if(EXISTS "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c")
      target_sources(test_rar PRIVATE
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
    endif()
    # Archives first, then their DSO deps (see --as-needed note above).
    target_link_libraries(test_rar PRIVATE
      -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
    if(SRSRAN4G_FFTW)
      target_link_libraries(test_rar PRIVATE ${SRSRAN4G_FFTW})
    endif()
    if(SRSRAN4G_MBEDTLS)
      target_link_libraries(test_rar PRIVATE ${SRSRAN4G_MBEDTLS})
    endif()
    target_link_libraries(test_rar PRIVATE m pthread)

    # ---- RAR PDSCH round-trip test (Step 7) ----
    # test_rar_pdsch.cpp encodes a MAC RAR through srsRAN_4G's PDSCH TX path
    # into a slot grid, IFFT-modulates it with our Ofdm, demodulates it back,
    # runs the LIVE Pdsch::demodulate() receiver path, then parses the MAC RAR.
    set(SRSRAN4G_PDSCH_TEST_SRC
      "${CMAKE_SOURCE_DIR}/tests/test_rar_pdsch.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
      "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
      "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
    )
    add_executable(test_rar_pdsch ${SRSRAN4G_PDSCH_TEST_SRC})
    target_compile_definitions(test_rar_pdsch PRIVATE GONE_HAVE_SRSRAN_OLD=1)
    target_include_directories(test_rar_pdsch PRIVATE
      "${CMAKE_SOURCE_DIR}/include"
      ${SRSRAN4G_INCLUDE_DIR})
    foreach(dir ${SRSRAN4G_LINK_DIRS})
      target_link_directories(test_rar_pdsch PRIVATE "${dir}")
    endforeach()
    if(EXISTS "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c")
      target_sources(test_rar_pdsch PRIVATE
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
        "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
    endif()
    # Archives first, then their DSO deps (see --as-needed note above).
    target_link_libraries(test_rar_pdsch PRIVATE
      -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
    if(SRSRAN4G_FFTW)
      target_link_libraries(test_rar_pdsch PRIVATE ${SRSRAN4G_FFTW})
    endif()
    if(SRSRAN4G_MBEDTLS)
      target_link_libraries(test_rar_pdsch PRIVATE ${SRSRAN4G_MBEDTLS})
    endif()
    target_link_libraries(test_rar_pdsch PRIVATE m pthread)

    # ---- PDCCH polar round-trip test ----
    # test_pdcch_rt.cpp encodes random DCI payloads with srsRAN's own PDCCH TX
    # (reference implementation), lays the symbols onto OUR candidate RE layout
    # (+ OUR DMRS, perfect channel), and runs OUR full Pdcch::process()
    # (DMRS channel est + LLRs + polar + CRC + RA-RNTI search). Proves the
    # decode glue correct on clean data — the path no test has ever exercised.
    # AL1 must survive with crc_ok (regression: the keep-gate once dropped all
    # AL-1 successes); AL2 is the control; noise must never CRC-pass.
    #
    # TEMPORARILY DISABLED (2026-09-17): the srsRAN tree linked on Latte
    # provides init_rx but not init_tx/set_carrier/encode (RX-only build?),
    # so the srsRAN-TX-based harness cannot link there yet. Options: (a) hand
    # roll TX from linked primitives (polar_encoder/rm_tx/mod tables — verify
    # presence first), or (b) compile the missing srsran TX object as an
    # explicit source like polar_chanalloc.c below. Re-enable after the
    # re-decode results below decide whether glue-doubt remains.
    # set(PDCCH_RT_TEST_SRC
    #   "${CMAKE_SOURCE_DIR}/tests/test_pdcch_rt.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
    #   "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
    # )
    # add_executable(test_pdcch_rt ${PDCCH_RT_TEST_SRC})
    # target_compile_definitions(test_pdcch_rt PRIVATE GONE_HAVE_SRSRAN_OLD=1)
    # target_include_directories(test_pdcch_rt PRIVATE
    #   "${CMAKE_SOURCE_DIR}/include"
    #   ${SRSRAN4G_INCLUDE_DIR})
    # foreach(dir ${SRSRAN4G_LINK_DIRS})
    #   target_link_directories(test_pdcch_rt PRIVATE "${dir}")
    # endforeach()
    # if(EXISTS "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c")
    #   target_sources(test_pdcch_rt PRIVATE
    #     "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
    #     "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
    # endif()
    # # Archives first, then their DSO deps (see --as-needed note above).
    # target_link_libraries(test_pdcch_rt PRIVATE
    #   -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
    # if(SRSRAN4G_FFTW)
    #   target_link_libraries(test_pdcch_rt PRIVATE ${SRSRAN4G_FFTW})
    # endif()
    # if(SRSRAN4G_MBEDTLS)
    #   target_link_libraries(test_pdcch_rt PRIVATE ${SRSRAN4G_MBEDTLS})
    # endif()
    # target_link_libraries(test_pdcch_rt PRIVATE m pthread)
  else()
    message(STATUS "5Gone: srsRAN 4G not found — correlation-only PDCCH decode (run scripts/install-srsran-4g.sh)")
  endif()
endif()

# --- Capture decode tool + test (Step 3) ---
# 5gone-decode reads a raw .cf32 capture, CellSync-locks on the SSB (optional),
# and runs the RAR decode chain over it. Both link the same offline decoder
# sources as the main binary (no UHD/yaml). When srsRAN-4G is present they
# inherit its DCI-bits decode path; otherwise correlation-only. `test_rar` only
# exists when srsRAN-4G was configured, so it is a reliable presence gate.
set(DECODE_TOOL_SRC
  "${CMAKE_SOURCE_DIR}/tools/5gone_decode.cpp"
  "${CMAKE_SOURCE_DIR}/tools/decode_capture.hpp"
  "${CMAKE_SOURCE_DIR}/src/cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp")
add_executable(5gone-decode ${DECODE_TOOL_SRC})
target_include_directories(5gone-decode PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}")
if(TARGET test_rar)
  target_compile_definitions(5gone-decode PRIVATE GONE_HAVE_SRSRAN_OLD=1)
  target_include_directories(5gone-decode PRIVATE ${SRSRAN4G_INCLUDE_DIR})
  target_link_directories(5gone-decode PRIVATE ${SRSRAN4G_LINK_DIRS})
  target_sources(5gone-decode PRIVATE
    "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
    "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
  target_link_libraries(5gone-decode PRIVATE
    -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
  if(SRSRAN4G_FFTW)
    target_link_libraries(5gone-decode PRIVATE ${SRSRAN4G_FFTW})
  endif()
  if(SRSRAN4G_MBEDTLS)
    target_link_libraries(5gone-decode PRIVATE ${SRSRAN4G_MBEDTLS})
  endif()
  target_link_libraries(5gone-decode PRIVATE m pthread)
else()
  if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
    target_compile_definitions(5gone-decode PRIVATE HAVE_LIQUID=1)
    target_include_directories(5gone-decode PRIVATE ${LIQUID_INCLUDE_DIR})
    target_link_libraries(5gone-decode PRIVATE ${LIQUID_LIBRARY})
  endif()
  target_link_libraries(5gone-decode PRIVATE m)
endif()
install(TARGETS 5gone-decode RUNTIME DESTINATION bin)

# --- Loopback RX dump analyzer (Step 4 debug) ---
# Self-contained: no UHD, no srsRAN. Reads a cf32 dump from run_loopback's
# dump_path and scans the WHOLE buffer for the marker burst, so we can tell
# whether RF coupling or timing-slip is the reason the harness missed it.
set(ANALYZE_LB_SRC
  "${CMAKE_SOURCE_DIR}/tools/analyze_lb.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
)
add_executable(analyze_lb ${ANALYZE_LB_SRC})
target_include_directories(analyze_lb PRIVATE "${CMAKE_SOURCE_DIR}/include")
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(analyze_lb PRIVATE HAVE_LIQUID=1)
  target_include_directories(analyze_lb PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(analyze_lb PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(analyze_lb PRIVATE m)

# --- Collide RX dump analyzer (debug: per-symbol msg2 clarity under CFO/phase) ---
set(ANALYZE_COLLIDE_SRC
  "${CMAKE_SOURCE_DIR}/tools/analyze_collide.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_tx.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/latency.cpp"
)
add_executable(analyze_collide ${ANALYZE_COLLIDE_SRC})
target_include_directories(analyze_collide PRIVATE "${CMAKE_SOURCE_DIR}/include")
target_link_libraries(analyze_collide PRIVATE m)

# --- Live RX probe (2-SDR PoC): lock the real gNB SSB on the attacker radio ---
# Pure RX (no TX) on the configured chain. Reuses config.cpp + radio_uhd.cpp so
# device serial / freq / gains come from rar_dos.yaml.
set(RX_PROBE_SRC
  "${CMAKE_SOURCE_DIR}/tools/rx_probe.cpp"
  "${CMAKE_SOURCE_DIR}/src/config.cpp"
  "${CMAKE_SOURCE_DIR}/src/radio_uhd.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
)
add_executable(rx_probe ${RX_PROBE_SRC})
target_include_directories(rx_probe PRIVATE include ${UHD_INCLUDE_DIRS})
target_link_libraries(rx_probe PRIVATE UHD::UHD yaml-cpp ${CMAKE_THREAD_LIBS_INIT})
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(rx_probe PRIVATE HAVE_LIQUID=1)
  target_include_directories(rx_probe PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(rx_probe PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(rx_probe PRIVATE m)

# --- Live TX probe: "does this radio radiate at all?" (no srsRAN) ---
set(TX_PROBE_SRC
  "${CMAKE_SOURCE_DIR}/tools/tx_probe.cpp"
  "${CMAKE_SOURCE_DIR}/src/config.cpp"
  "${CMAKE_SOURCE_DIR}/src/radio_uhd.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
)
add_executable(tx_probe ${TX_PROBE_SRC})
target_include_directories(tx_probe PRIVATE include ${UHD_INCLUDE_DIRS})
target_link_libraries(tx_probe PRIVATE UHD::UHD yaml-cpp ${CMAKE_THREAD_LIBS_INIT})
if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
  target_compile_definitions(tx_probe PRIVATE HAVE_LIQUID=1)
  target_include_directories(tx_probe PRIVATE ${LIQUID_INCLUDE_DIR})
  target_link_libraries(tx_probe PRIVATE ${LIQUID_LIBRARY})
endif()
target_link_libraries(tx_probe PRIVATE m)

set(DECODE_TEST_SRC
  "${CMAKE_SOURCE_DIR}/tests/test_decode_tool.cpp"
  "${CMAKE_SOURCE_DIR}/tools/decode_capture.hpp"
  "${CMAKE_SOURCE_DIR}/src/cell_sync.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_rar_decoder.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdcch.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dci.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dmrs.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pn.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_dsp.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_symbol.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_ofdm.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pdsch.cpp"
  "${CMAKE_SOURCE_DIR}/src/mac_rar.cpp"
  "${CMAKE_SOURCE_DIR}/src/nr_pss.cpp")
add_executable(test_decode_tool ${DECODE_TEST_SRC})
target_include_directories(test_decode_tool PRIVATE
  "${CMAKE_SOURCE_DIR}/include"
  "${CMAKE_SOURCE_DIR}")
if(TARGET test_rar)
  target_compile_definitions(test_decode_tool PRIVATE GONE_HAVE_SRSRAN_OLD=1)
  target_include_directories(test_decode_tool PRIVATE ${SRSRAN4G_INCLUDE_DIR})
  target_link_directories(test_decode_tool PRIVATE ${SRSRAN4G_LINK_DIRS})
  target_sources(test_decode_tool PRIVATE
    "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_chanalloc.c"
    "${SRSRAN4G_SRC}/lib/src/phy/fec/polar/polar_interleaver.c")
  target_link_libraries(test_decode_tool PRIVATE
    -Wl,--start-group srsran_common srsran_phy srsran_mac srslog support srsran_asn1 asn1_utils -Wl,--end-group)
  if(SRSRAN4G_FFTW)
    target_link_libraries(test_decode_tool PRIVATE ${SRSRAN4G_FFTW})
  endif()
  if(SRSRAN4G_MBEDTLS)
    target_link_libraries(test_decode_tool PRIVATE ${SRSRAN4G_MBEDTLS})
  endif()
  target_link_libraries(test_decode_tool PRIVATE m pthread)
else()
  if(LIQUID_LIBRARY AND LIQUID_INCLUDE_DIR)
    target_compile_definitions(test_decode_tool PRIVATE HAVE_LIQUID=1)
    target_include_directories(test_decode_tool PRIVATE ${LIQUID_INCLUDE_DIR})
    target_link_libraries(test_decode_tool PRIVATE ${LIQUID_LIBRARY})
  endif()
  target_link_libraries(test_decode_tool PRIVATE m)
endif()

target_link_libraries(5gone-rar-dos PRIVATE
  UHD::UHD
  yaml-cpp
  Threads::Threads
)

if(5GONE_ENABLE_SRSRAN)
  set(SRSRAN_BUILD_DIR "$ENV{SRSRAN_BUILD}" CACHE PATH "srsRAN Project build dir")
  if(NOT SRSRAN_BUILD_DIR)
    set(SRSRAN_BUILD_DIR "${CMAKE_SOURCE_DIR}/../build/srsran_build")
  endif()
  if(EXISTS "${SRSRAN_BUILD_DIR}/lib/libsrsran_phy.a" OR EXISTS "${SRSRAN_BUILD_DIR}/lib/libsrsran_phy.so")
    target_compile_definitions(5gone-rar-dos PRIVATE HAVE_SRSRAN=1)
    target_include_directories(5gone-rar-dos PRIVATE "${SRSRAN_BUILD_DIR}/../srsran_project/include")
    target_link_directories(5gone-rar-dos PRIVATE "${SRSRAN_BUILD_DIR}/lib")
    target_link_libraries(5gone-rar-dos PRIVATE srsran_phy srsran_common srslog)
    message(STATUS "5Gone: srsRAN PHY linked from ${SRSRAN_BUILD_DIR}")
  else()
    message(STATUS "5Gone: srsRAN PHY not found — template PUSCH encoder only")
  endif()
endif()

install(TARGETS 5gone-rar-dos RUNTIME DESTINATION bin)
