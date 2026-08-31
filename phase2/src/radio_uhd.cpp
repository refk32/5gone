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
};

RadioUhd::RadioUhd(const AttackConfig& cfg) : cfg_(cfg), impl_(std::make_unique<Impl>())
{
  impl_->usrp = uhd::usrp::multi_usrp::make(cfg_.device_args);
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
  }
}

} // namespace gone
