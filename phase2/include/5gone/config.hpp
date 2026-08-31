#pragma once

#include "5gone/types.hpp"
#include <string>

namespace gone {

AttackConfig load_config(const std::string& yaml_path);
AttackConfig load_config_with_overrides(const std::string& yaml_path, int argc, char** argv);

} // namespace gone
