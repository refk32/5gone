#include "5gone/sample_clock.hpp"

#include <cmath>

namespace gone {

uint64_t rx_sample_from_time(double t_sec, double sample_rate)
{
    return static_cast<uint64_t>(std::llround(t_sec * sample_rate));
}

double rx_time_from_sample(uint64_t sample_index, double sample_rate)
{
    return static_cast<double>(sample_index) / sample_rate;
}

} // namespace gone