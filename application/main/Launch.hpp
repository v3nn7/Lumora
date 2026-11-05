#pragma once

#include "common/ModuleExport.hpp"

#define LUMORA_LAUNCH_API LUMORA_SHELL_API

namespace lumora::app {

enum class Flavor {
    AllInOne,  // development build: Hub and editor in one process
    Editor,    // packaged editor (one installed version)
    Hub,       // Hub only: opens projects in installed editors
};

/// The whole application behind main() (in the shell module).
LUMORA_LAUNCH_API int runLumora(int argc, char** argv, Flavor flavor);

} // namespace lumora::app
