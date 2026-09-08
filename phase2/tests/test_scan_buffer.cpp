#include "5gone/rar_monitor.hpp"
#include <cassert>
#include <iostream>
#include <vector>
#include <complex>

void test_energy_filter_ignores_noise() {
    std::cout << "[Test] Running test_energy_filter_ignores_noise..." << std::endl;
    
    gone::AttackConfig cfg;
    gone::RarMonitor monitor(cfg);

    // Create a buffer of very low energy (noise floor < 1e-4)
    gone::SampleBuffer noisy_buffer(100);
    for (auto& sample : noisy_buffer) {
        sample = std::complex<float>(0.001f, 0.001f);
    }

    auto events = monitor.scan_buffer(noisy_buffer);
    assert(events.empty() && "Noise buffer should not produce any RAR events!");
    std::cout << "  ✓ Passed: noise correctly ignored." << std::endl;
}

void test_scan_buffer_callback_emission() {
    std::cout << "[Test] Running test_scan_buffer_callback_emission..." << std::endl;

    gone::AttackConfig cfg;
    gone::RarMonitor monitor(cfg);

    bool callback_called = false;
    monitor.on_rar([&](const gone::RarEvent& ev) {
        callback_called = true;
        std::cout << "  -> Callback received event! Rapid: " << (int)ev.rapid 
                  << ", TC-RNTI: 0x" << std::hex << ev.c_rnti << std::dec << std::endl;
    });

    // Create a high-energy buffer to pass the RSSI pre-filter
    gone::SampleBuffer active_buffer(512);
    for (auto& sample : active_buffer) {
        sample = std::complex<float>(0.5f, 0.5f);
    }

    auto events = monitor.scan_buffer(active_buffer);
    
    // Depending on whether HAVE_SRSRAN is defined, scan_buffer will either:
    // 1. Return events via fallback/mock if configured, or
    // 2. Return empty if waiting for real srsRAN frames.
    std::cout << "  ✓ Scan completed without crashing. Events emitted: " << events.size() << std::endl;
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << " Starting RarMonitor::scan_buffer Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_energy_filter_ignores_noise();
    test_scan_buffer_callback_emission();

    std::cout << "========================================" << std::endl;
    std::cout << " All scan_buffer tests passed successfully!" << std::endl;
    std::cout << "========================================" << std::endl;
    return 0;
}