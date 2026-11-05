// Single translation unit compiling the cgltf implementation.
#include <charconv>
#include <cstdlib>
#include <cstring>

namespace {
// cgltf parses JSON numbers with atof/atoi by default, which honour the process locale. Qt (and
// any GUI toolkit) switches LC_NUMERIC to the user's locale, where "0.25" can parse as 0 (decimal
// comma). JSON numbers are always '.'-separated, so parse them locale-independently.
double lumoraCgltfAtof(const char* text) {
    double value = 0.0;
    std::from_chars(text, text + std::strlen(text), value);
    return value;
}
int lumoraCgltfAtoi(const char* text) {
    int value = 0;
    std::from_chars(text, text + std::strlen(text), value);
    return value;
}
long long lumoraCgltfAtoll(const char* text) {
    long long value = 0;
    std::from_chars(text, text + std::strlen(text), value);
    return value;
}
} // namespace

#define CGLTF_ATOF(str) lumoraCgltfAtof(str)
#define CGLTF_ATOI(str) lumoraCgltfAtoi(str)
#define CGLTF_ATOLL(str) lumoraCgltfAtoll(str)
#define CGLTF_IMPLEMENTATION
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wimplicit-fallthrough"
#pragma GCC diagnostic ignored "-Wcast-align"
#endif
#include <cgltf.h>
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
