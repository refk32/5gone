#include "5gone/radio_uhd.hpp"
#include <uhd/usrp/multi_usrp.hpp>
#include <uhd/types/tune_request.hpp>
#include <iostream>
#include <thread>

namespace gone {

struct RadioUhd::Impl {
  uhd::usrp::multi_usrp::sptr usrp;
  uhd::tx_streamer::sptr tx_stream;
  uhd::rx_streamer::sptr rx_stream;
  bool streaming{false};
  uint64_t rx_overflow{0};   // UHD RX overflow events (host couldn't keep up)
  uint64_t rx_lost{0};       // sequence errors / lost packets on the RX stream
  uint64_t tx_underrun{0};   // TX packets that the device refused
};

RadioUhd::RadioUhd(const AttackConfig& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>())
{
  impl_->usrp = uhd::usrp::multi_usrp::make(cfg_.device_args);

  // Configure the chain mapping FIRST. On B2xx, changing the subdev/antenna
  // reinitializes the frontend and RESETS gains to 0 — so gains (and only
  // then rate/freq) must be set after the subdev/antenna are final.
  try {
    impl_->usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t(cfg_.tx_subdev));
    impl_->usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t(cfg_.rx_subdev));
  } catch (const std::exception& e) {
    std::cerr << "[radio] subdev spec '" << cfg_.tx_subdev << "'/'" << cfg_.rx_subdev
              << "': " << e.what() << " (defaulting to A:A/A:B)\n";
    impl_->usrp->set_tx_subdev_spec(uhd::usrp::subdev_spec_t("A:A"));
    impl_->usrp->set_rx_subdev_spec(uhd::usrp::subdev_spec_t("A:B"));
  }
  try {
    impl_->usrp->set_tx_antenna(cfg_.tx_antenna);
    impl_->usrp->set_rx_antenna(cfg_.rx_antenna);
  } catch (const std::exception& e) {
    std::cerr << "[radio] antenna '" << cfg_.tx_antenna << "'/'" << cfg_.rx_antenna
              << "': " << e.what() << " (defaulting to TX/RX)\n";
    impl_->usrp->set_tx_antenna("TX/RX");
    impl_->usrp->set_rx_antenna("TX/RX");
  }

  impl_->usrp->set_rx_rate(cfg_.sample_rate);
  impl_->usrp->set_tx_rate(cfg_.sample_rate);
  impl_->usrp->set_rx_freq(uhd::tune_request_t(cfg_.center_freq_hz));
  impl_->usrp->set_tx_freq(uhd::tune_request_t(cfg_.center_freq_hz));
  impl_->usrp->set_rx_gain(cfg_.rx_gain);
  impl_->usrp->set_tx_gain(cfg_.tx_gain);

  uhd::stream_args_t tx_args("fc32", "sc16");
  impl_->tx_stream = impl_->usrp->get_tx_stream(tx_args);

  uhd::stream_args_t rx_args("fc32", "sc16");
  impl_->rx_stream = impl_->usrp->get_rx_stream(rx_args);
}

RadioUhd::~RadioUhd()
{
  stop_streaming();
}

void RadioUhd::start_streaming()
{
  if (impl_->streaming) return;
  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_START_CONTINUOUS);
  cmd.stream_now = true;
  impl_->rx_stream->issue_stream_cmd(cmd);
  impl_->streaming = true;
}

void RadioUhd::stop_streaming()
{
  if (!impl_->streaming) return;
  uhd::stream_cmd_t cmd(uhd::stream_cmd_t::STREAM_MODE_STOP_CONTINUOUS);
  impl_->rx_stream->issue_stream_cmd(cmd);
  impl_->streaming = false;
}

std::size_t RadioUhd::recv(SampleBuffer& out, double timeout_sec)
{
  if (out.empty()) out.resize(4096);
  uhd::rx_metadata_t md;
  const auto n = impl_->rx_stream->recv(out.data(), out.size(), md, timeout_sec);
  if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
    return 0;
  }
  return n;
}

std::size_t RadioUhd::recv_timed(SampleBuffer& out, double timeout_sec, bool& got_time,
                                 double& rx_time_sec)
{
  if (out.empty()) out.resize(4096);
  uhd::rx_metadata_t md;
  const auto n = impl_->rx_stream->recv(out.data(), out.size(), md, timeout_sec);
  if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_NONE) {
    got_time = false;
    if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_OVERFLOW) ++impl_->rx_overflow;
    else if (md.error_code == uhd::rx_metadata_t::ERROR_CODE_TIMEOUT) { }
    else if (md.error_code != uhd::rx_metadata_t::ERROR_CODE_LATE_COMMAND) ++impl_->rx_lost;
    return 0;
  }
  got_time = md.has_time_spec;
  rx_time_sec = md.time_spec.get_real_secs();
  return n;
}

void RadioUhd::transmit(const SampleBuffer& iq, double delay_sec)
{
  if (iq.empty()) return;
  if (delay_sec > 0) {
    std::this_thread::sleep_for(std::chrono::duration<double>(delay_sec));
  }

  uhd::tx_metadata_t md;
  md.has_time_spec = false;
  md.start_of_burst = true;
  md.end_of_burst = true;

  const auto sent = impl_->tx_stream->send(iq.data(), iq.size(), md);
  if (sent != iq.size()) {
    std::cerr << "[radio] TX underrun: sent " << sent << "/" << iq.size() << "\n";
    ++impl_->tx_underrun;
  }
}

void RadioUhd::transmit_seg(const SampleBuffer& iq, bool start_of_burst, bool end_of_burst)
{
  if (iq.empty()) return;
  uhd::tx_metadata_t md;
  md.has_time_spec = false;
  md.start_of_burst = start_of_burst;
  md.end_of_burst = end_of_burst;
  const auto sent = impl_->tx_stream->send(iq.data(), iq.size(), md);
  if (sent != iq.size())
    std::cerr << "[radio] TX packet underrun: sent " << sent << "/" << iq.size() << "\n";
}

void RadioUhd::sync_time(double t_sec)
{
  impl_->usrp->set_time_now(uhd::time_spec_t(t_sec));
}

void RadioUhd::transmit_timed(const SampleBuffer& iq, double abs_time_sec)
{
  if (iq.empty()) return;
  uhd::tx_metadata_t md;
  md.has_time_spec = true;
  md.time_spec = uhd::time_spec_t(abs_time_sec);
  md.start_of_burst = true;
  md.end_of_burst = true;

  const auto sent = impl_->tx_stream->send(iq.data(), iq.size(), md);
  if (sent != iq.size()) {
    std::cerr << "[radio] timed TX underrun: sent " << sent << "/" << iq.size() << "\n";
    ++impl_->tx_underrun;
  }
}

double RadioUhd::uhd_now_sec() const
{
  return impl_->usrp->get_time_now().get_real_secs();
}

uint64_t RadioUhd::rx_overflow_count() const { return impl_->rx_overflow; }
uint64_t RadioUhd::rx_lost_count() const { return impl_->rx_lost; }
uint64_t RadioUhd::tx_underrun_count() const { return impl_->tx_underrun; }

double RadioUhd::get_tx_gain() const
{
  try { return impl_->usrp->get_tx_gain(); } catch (...) { return -1.0; }
}

double RadioUhd::get_rx_gain() const
{
  try { return impl_->usrp->get_rx_gain(); } catch (...) { return -1.0; }
}

double RadioUhd::get_tx_freq_hz() const
{
  try { return impl_->usrp->get_tx_freq(); } catch (...) { return -1.0; }
}

double RadioUhd::get_rx_freq_hz() const
{
  try { return impl_->usrp->get_rx_freq(); } catch (...) { return -1.0; }
}

std::string RadioUhd::get_tx_antenna() const
{
  try { return impl_->usrp->get_tx_antenna(); } catch (...) { return "?"; }
}

std::string RadioUhd::get_rx_antenna() const
{
  try { return impl_->usrp->get_rx_antenna(); } catch (...) { return "?"; }
}

std::string RadioUhd::get_tx_sensor(const std::string& name) const
{
  try { return impl_->usrp->get_tx_sensor(name).to_pp_string(); }
  catch (...) { return "n/a"; }
}

std::string RadioUhd::get_rx_sensor(const std::string& name) const
{
  try { return impl_->usrp->get_rx_sensor(name).to_pp_string(); }
  catch (...) { return "n/a"; }
}

double RadioUhd::get_temp_c() const
{
  try { return impl_->usrp->get_mboard_sensor("temp").to_real(); }
  catch (...) { return -1.0; }
}

} // namespace gone
