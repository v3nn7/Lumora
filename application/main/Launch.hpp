#pragma once

#if defined(_WIN32)
#if defined(LUMORA_BUILDING_LIBRARY)
#define LUMORA_LAUNCH_API __declspec(dllexport)
#else
#define LUMORA_LAUNCH_API __declspec(dllimport)
#endif
#else
#define LUMORA_LAUNCH_API __attribute__((visibility("default")))
#endif

namespace lumora::app {

enum class Flavor {
    AllInOne,  // development build: Hub and editor in one process
    Editor,    // packaged editor (one installed version)
    Hub,       // Hub only: opens projects in installed editors
};

/// The whole application behind main(); the only symbol the shared library exports.
LUMORA_LAUNCH_API int runLumora(int argc, char** argv, Flavor flavor);

} // namespace lumora::app
