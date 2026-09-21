#pragma once

#include "retention_model.hpp"

#include <string>

namespace rtmem {

BankConfigs load_device_profile(const std::string& path);

}  // namespace rtmem
