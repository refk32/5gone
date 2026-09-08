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
  else()
    message(STATUS "5Gone: srsRAN 4G not found — correlation-only PDCCH decode (run scripts/install-srsran-4g.sh)")
  endif()
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
