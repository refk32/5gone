#include "5gone/config.hpp"
#include <yaml-cpp/yaml.h>
#include <cstring>
#include <iostream>
#include <stdexcept>

namespace gone {

static std::string yaml_str(const YAML::Node& n, const char* key, const std::string& def)
{
  if (n[key]) return n[key].as<std::string>();
  return def;
}

static double yaml_dbl(const YAML::Node& n, const char* key, double def)
{
  if (n[key]) return n[key].as<double>();
  return def;
}

static bool yaml_bool(const YAML::Node& n, const char* key, bool def)
{
  if (n[key]) return n[key].as<bool>();
  return def;
}

AttackConfig load_config(const std::string& yaml_path)
{
  YAML::Node root = YAML::LoadFile(yaml_path);
  AttackConfig cfg;

  if (root["mode"]) cfg.mode = root["mode"].as<std::string>();

  if (root["radio"]) {
    const auto& r = root["radio"];
    cfg.sample_rate = yaml_dbl(r, "sample_rate", cfg.sample_rate);
    cfg.center_freq_hz = yaml_dbl(r, "center_freq_hz", cfg.center_freq_hz);
    cfg.tx_gain = yaml_dbl(r, "tx_gain", cfg.tx_gain);
    cfg.rx_gain = yaml_dbl(r, "rx_gain", cfg.rx_gain);
    cfg.device_args = yaml_str(r, "device_args", cfg.device_args);
    cfg.tx_subdev = yaml_str(r, "tx_subdev", cfg.tx_subdev);
    cfg.rx_subdev = yaml_str(r, "rx_subdev", cfg.rx_subdev);
    cfg.tx_antenna = yaml_str(r, "tx_antenna", cfg.tx_antenna);
    cfg.rx_antenna = yaml_str(r, "rx_antenna", cfg.rx_antenna);
  }

  if (root["cell"]) {
    const auto& c = root["cell"];
    if (c["pci"]) cfg.pci = c["pci"].as<uint8_t>();
    if (c["scs_khz"]) cfg.scs_khz = c["scs_khz"].as<uint8_t>();
    if (c["bandwidth_mhz"]) cfg.bandwidth_mhz = c["bandwidth_mhz"].as<uint16_t>();
    if (c["band"]) cfg.band = c["band"].as<uint8_t>();
    if (c["pss_bin_shift"]) cfg.pss_bin_shift = c["pss_bin_shift"].as<int>();
    if (c["dl_arfcn"]) {
      // n78 ARFCN → approx center (lab uses gNB config dl_arfcn 632628)
      uint32_t arfcn = c["dl_arfcn"].as<uint32_t>();
      (void)arfcn;
    }
  }

  if (root["attack"]) {
    const auto& a = root["attack"];
    cfg.symbol_advance_us = yaml_dbl(a, "symbol_advance_us", cfg.symbol_advance_us);
    cfg.tx_power_scale = yaml_dbl(a, "tx_power_scale", cfg.tx_power_scale);
    cfg.live_fire_corr = static_cast<float>(yaml_dbl(a, "live_fire_corr", cfg.live_fire_corr));
    if (a["tc_rnti_seed"]) cfg.live_tc_rnti_seed = a["tc_rnti_seed"].as<uint16_t>();
  }

  if (root["loopback"]) {
    const auto& lb = root["loopback"];
    cfg.loopback_iterations = lb["iterations"]
        ? lb["iterations"].as<uint32_t>() : cfg.loopback_iterations;
    cfg.loopback_window_ms = yaml_dbl(lb, "window_ms", cfg.loopback_window_ms);
    cfg.loopback_tx_scale = yaml_dbl(lb, "tx_scale", cfg.loopback_tx_scale);
    cfg.loopback_dump_path = yaml_str(lb, "dump_path", cfg.loopback_dump_path);
    cfg.loopback_probe = yaml_bool(lb, "probe", cfg.loopback_probe);
  }

  if (root["prach"]) {
    const auto& p = root["prach"];
    cfg.prach.enabled = yaml_bool(p, "enabled", cfg.prach.enabled);
    if (p["rapid"]) cfg.prach.rapid = p["rapid"].as<uint16_t>();
    cfg.prach.cycle_rapids = yaml_bool(p, "cycle_rapids", cfg.prach.cycle_rapids);
    if (p["root_sequence_index"]) cfg.prach.root_sequence_index = p["root_sequence_index"].as<uint16_t>();
    if (p["zero_correlation_zone"]) cfg.prach.zero_correlation_zone = p["zero_correlation_zone"].as<uint8_t>();
    if (p["msg1_frequency_start_prb"]) cfg.prach.msg1_frequency_start_prb = p["msg1_frequency_start_prb"].as<uint16_t>();
    if (p["occasion_slot"]) cfg.prach.occasion_slot = p["occasion_slot"].as<uint8_t>();
    if (p["occasion_count"]) cfg.prach.occasion_count = p["occasion_count"].as<uint32_t>();
    cfg.prach.sweep_occasion_slots = yaml_bool(p, "sweep_occasion_slots", cfg.prach.sweep_occasion_slots);
    cfg.prach.auto_cfo = yaml_bool(p, "auto_cfo", cfg.prach.auto_cfo);
    cfg.prach.cfo_comp_hz = yaml_dbl(p, "cfo_comp_hz", cfg.prach.cfo_comp_hz);
    cfg.prach.dither_cfo = yaml_bool(p, "dither_cfo", cfg.prach.dither_cfo);
    cfg.prach.dither_halfspan_hz = yaml_dbl(p, "dither_halfspan_hz", cfg.prach.dither_halfspan_hz);
    cfg.prach.dither_step_hz = yaml_dbl(p, "dither_step_hz", cfg.prach.dither_step_hz);
    cfg.prach.tx_gain_db = yaml_dbl(p, "tx_gain_db", cfg.prach.tx_gain_db);
    cfg.prach.rar_capture_dir = p["rar_capture_dir"] ? p["rar_capture_dir"].as<std::string>() : std::string();
    if (p["rar_capture_window_slots"])
      cfg.prach.rar_capture_window_slots = p["rar_capture_window_slots"].as<uint32_t>();
    if (p["rar_capture_pre_slots"])
      cfg.prach.rar_capture_pre_slots = p["rar_capture_pre_slots"].as<uint32_t>();
    cfg.prach.rar_capture_drain_sec = yaml_dbl(p, "rar_capture_drain_sec", cfg.prach.rar_capture_drain_sec);
    cfg.prach.live_scan = yaml_bool(p, "live_scan", cfg.prach.live_scan);
  } else {
    // No `prach:` node: default-off, so zero-change for existing configs.
    cfg.prach.enabled = false;
  }

  if (root["collide"]) {
    const auto& c = root["collide"];
    cfg.collide_iterations = c["iterations"]
        ? c["iterations"].as<uint32_t>() : cfg.collide_iterations;
    cfg.collide_legit_scale = yaml_dbl(c, "legit_scale", cfg.collide_legit_scale);
    cfg.collide_attack_scale = yaml_dbl(c, "attack_scale", cfg.collide_attack_scale);
    cfg.collide_dump_path = yaml_str(c, "dump_path", cfg.collide_dump_path);
    if (c["delay_symbols"] && c["delay_symbols"].IsSequence()) {
      std::vector<double> deltas;
      for (const auto& d : c["delay_symbols"]) deltas.push_back(d.as<double>());
      if (!deltas.empty()) cfg.collide_delay_symbols = std::move(deltas);
    }
  }

  if (root["paths"]) {
    const auto& p = root["paths"];
    cfg.iq_template_dir = yaml_str(p, "iq_template_dir", cfg.iq_template_dir);
    cfg.grant_file = yaml_str(p, "grant_file", cfg.grant_file);
    cfg.dataset_dir = yaml_str(p, "dataset_dir", cfg.dataset_dir);
    cfg.log_file = yaml_str(p, "log_file", cfg.log_file);
    cfg.grant_bus = yaml_str(p, "grant_bus", cfg.grant_bus);
  }

  cfg.dry_run = yaml_bool(root, "dry_run", cfg.dry_run);
  return cfg;
}

AttackConfig load_config_with_overrides(const std::string& yaml_path, int argc, char** argv)
{
  AttackConfig cfg = load_config(yaml_path);
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
      cfg.mode = argv[++i];
    } else if (std::strcmp(argv[i], "--grant") == 0 && i + 1 < argc) {
      cfg.grant_file = argv[++i];
    } else if (std::strcmp(argv[i], "--dataset") == 0 && i + 1 < argc) {
      cfg.dataset_dir = argv[++i];
    } else if (std::strcmp(argv[i], "--dry-run") == 0) {
      cfg.dry_run = true;
    } else if (std::strcmp(argv[i], "--device") == 0 && i + 1 < argc) {
      cfg.device_args = argv[++i];
    } else if (std::strcmp(argv[i], "--symbol-advance-us") == 0 && i + 1 < argc) {
      cfg.symbol_advance_us = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--tx-subdev") == 0 && i + 1 < argc) {
      cfg.tx_subdev = argv[++i];
    } else if (std::strcmp(argv[i], "--rx-subdev") == 0 && i + 1 < argc) {
      cfg.rx_subdev = argv[++i];
    } else if (std::strcmp(argv[i], "--loop-iterations") == 0 && i + 1 < argc) {
      cfg.loopback_iterations = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (std::strcmp(argv[i], "--occasion-slot") == 0 && i + 1 < argc) {
      cfg.prach.occasion_slot = static_cast<uint8_t>(std::stoul(argv[++i]));
    } else if (std::strcmp(argv[i], "--occasion-count") == 0 && i + 1 < argc) {
      cfg.prach.occasion_count = static_cast<uint32_t>(std::stoul(argv[++i]));
    } else if (std::strcmp(argv[i], "--sweep-occasion-slots") == 0) {
      cfg.prach.sweep_occasion_slots = true;
    } else if (std::strcmp(argv[i], "--cfo") == 0 && i + 1 < argc) {
      cfg.prach.cfo_comp_hz = std::stod(argv[++i]);
    } else if (std::strcmp(argv[i], "--dither-cfo") == 0) {
      cfg.prach.dither_cfo = true;
    } else if (std::strcmp(argv[i], "--no-live-scan") == 0) {
      cfg.prach.live_scan = false;
    } else if (std::strcmp(argv[i], "--no-dither-cfo") == 0) {
      cfg.prach.dither_cfo = false;
    } else if (std::strcmp(argv[i], "--help") == 0 || std::strcmp(argv[i], "-h") == 0) {
      std::cout << "5gone-rar-dos — RAR DoS uplink overshadow (Section 4.1)\n"
                << "  --mode sim|inject|live|bus|loopback|collide\n"
                << "  --grant FILE.json\n"
                << "  --dataset DIR\n"
                << "  --device UHD_ARGS\n"
                << "  --symbol-advance-us US\n"
                << "  --tx-subdev SPEC  (loopback; default A:A)\n"
                << "  --rx-subdev SPEC  (loopback; default A:B)\n"
                << "  --loop-iterations N\n"
                << "  --occasion-slot N     PRACH occasion slot-in-frame (0..19)\n"
                << "  --occasion-count N    occasions to send on\n"
                << "  --sweep-occasion-slots  cycle slot 0..19 across sends (one lock)\n"
                << "  --cfo HZ              TX CFO compensation (comb offset)\n"
                << "  --dither-cfo | --no-dither-cfo\n"
                << "  --no-live-scan      skip live RAR monitor (pure capture runs)\n"
                << "  --dry-run\n";
      std::exit(0);
    }
  }
  return cfg;
}

} // namespace gone
