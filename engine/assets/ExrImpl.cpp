// tinyexr implementation (OpenEXR reading for environment maps), with miniz for ZIP compression.
// Third-party code: compiled without warnings (see engine/CMakeLists.txt).
#define TINYEXR_IMPLEMENTATION
#define TINYEXR_USE_MINIZ 1
#define TINYEXR_USE_THREAD 0
#define TINYEXR_USE_OPENMP 0
#include <tinyexr.h>
