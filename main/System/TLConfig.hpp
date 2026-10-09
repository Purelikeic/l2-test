#pragma once

#include "../Base/TLLocal.hpp"
#include <functional>
#include <string>

// Shared existing default/INI/seed order; protocol and plugin setup stay outside.
TLLocalConfig LoadTestConfig(std::function<void(TLLocalConfig&)> initialize,
                             const std::string& iniPath = "tltest.ini");
