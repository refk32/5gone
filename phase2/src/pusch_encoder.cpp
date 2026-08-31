#include "5gone/pusch_encoder.hpp"
#include <cmath>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace gone {

PuschEncoder::PuschEncoder(const AttackConfig& cfg) : cfg_(cfg) {}

SampleBuffer PuschEncoder::fallback_qpsk(const UlGrant& grant, const std::vector<uint8_t>& tb) const
{
  // Lab fallback: map TB bits → QPSK constellation, repeat to fill ~1 slot of samples.
  // Full LDPC/DMRS requires srsRAN PHY — use iq_templates/ for production overshadow.
  (void)grant;
  SampleBuffer iq;
  const std::size_t sym_bits = tb.size() * 8;
  const std::size_t qpsk_syms = std::max<std::size_t>(sym_bits / 2, 66); // paper: nof_re=132 → 66 QPSK
  iq.reserve(qpsk_syms * 64);

  auto bit = [&](std::size_t i) -> float {
    std::size_t byte_i = i / 8;
    if (byte_i >= tb.size()) return 0.f;
    return ((tb[byte_i] >> (7 - (i % 8))) & 1) ? 1.f : -1.f;
  };

  for (std::size_t s = 0; s < qpsk_syms; ++s) {
    float i = bit(s * 2);
    float q = bit(s * 2 + 1);
    const float scale = static_cast<float>(cfg_.tx_power_scale / std::sqrt(2.0));
    for (int rep = 0; rep < 64; ++rep) {
      iq.emplace_back(i * scale, q * scale);
    }
  }
  return iq;
}

SampleBuffer PuschEncoder::load_template(const UlGrant& grant) const
{
  std::ostringstream name;
  name << "mcs" << static_cast<int>(grant.mcs)
       << "_freq" << grant.pusch_freq_res
       << "_k" << static_cast<int>(grant.k) << ".cf32";

  const std::string path = cfg_.iq_template_dir + "/" + name.str();
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    return {};
  }

  in.seekg(0, std::ios::end);
  const auto bytes = static_cast<std::size_t>(in.tellg());
  in.seekg(0, std::ios::beg);

  SampleBuffer iq(bytes / sizeof(Sample));
  in.read(reinterpret_cast<char*>(iq.data()), static_cast<std::streamsize>(bytes));
  return iq;
}

SampleBuffer PuschEncoder::encode(const UlGrant& grant, const std::vector<uint8_t>& tb) const
{
  auto templ = load_template(grant);
  if (!templ.empty()) {
    return templ;
  }

#ifdef HAVE_SRSRAN
  // srsRAN PHY integration point — link full NR PUSCH when libs available.
  (void)tb;
  std::cerr << "[pusch] srsRAN linked but template path missing — using fallback QPSK\n";
#endif

  return fallback_qpsk(grant, tb);
}

} // namespace gone
