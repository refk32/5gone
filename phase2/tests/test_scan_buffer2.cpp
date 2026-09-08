#include "5gone/rar_monitor.hpp"
#include <iostream>
#include <fstream>

int main() {
    gone::AttackConfig cfg;
    gone::RarMonitor monitor(cfg);

    monitor.on_rar([](const gone::RarEvent& ev) {
        std::cout << "[SIM-DECODE] Successfully decoded RAR! TC-RNTI: 0x" 
                  << std::hex << ev.c_rnti << std::dec 
                  << " RAPID: " << (int)ev.rapid << std::endl;
    });

    // Simulate receiving IQ chunks (e.g. from file or looped playback)
    gone::SampleBuffer mock_stream_chunk(4096);
    for (auto& s : mock_stream_chunk) {
        s = std::complex<float>(0.8f, 0.8f); // High energy passing RSSI filter
    }

    std::cout << "[*] Starting live buffer scan simulation..." << std::endl;
    for (int i = 0; i < 5; ++i) {
        auto events = monitor.scan_buffer(mock_stream_chunk);
        std::cout << "Iteration " << i << " -> Events detected: " << events.size() << std::endl;
    }

    return 0;
}