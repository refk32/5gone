#include "5gone/attack_engine.hpp"
#include "5gone/config.hpp"
#include <csignal>
#include <iostream>
#include <string>

static gone::AttackEngine* g_engine = nullptr;

static void on_signal(int)
{
  if (g_engine) g_engine->request_stop();
}

int main(int argc, char** argv)
{
  const std::string config_path = (argc > 1 && argv[1][0] != '-')
      ? argv[1] : "phase2/config/rar_dos.yaml";

  try {
    auto cfg = gone::load_config_with_overrides(config_path, argc, argv);
    gone::AttackEngine engine(std::move(cfg));
    g_engine = &engine;
    std::signal(SIGINT, on_signal);
    return engine.run();
  } catch (const std::exception& e) {
    std::cerr << "fatal: " << e.what() << "\n";
    return 1;
  }
}
