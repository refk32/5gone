cmake_minimum_required(VERSION 3.16)
project(5gone_attacker VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(UHD REQUIRED)
find_package(yaml-cpp REQUIRED)
find_package(Threads REQUIRED)

option(5GONE_ENABLE_SRSRAN "Link srsRAN Project PHY libs if available" ON)

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
)

add_executable(5gone-rar-dos ${5GONE_SOURCES})
target_include_directories(5gone-rar-dos PRIVATE include)

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
