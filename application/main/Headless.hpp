#pragma once

#include "main/CommandLine.hpp"

namespace lumora::app {

/// Automation/build mode: no windows, no swapchain. Returns the process exit code.
[[nodiscard]] int runHeadless(const AppOptions& options);

} // namespace lumora::app
