#pragma once

#include "core/Log.hpp"

#include <cstdlib>
#include <source_location>

namespace lumora::detail {
[[noreturn]] inline void assertionFailed(const char* expr, std::string_view message, std::source_location loc) {
    Logger::instance().log(LogLevel::Fatal, "Assert",
                           std::format("{} failed: {} ({}:{} in {})", expr, message, loc.file_name(), loc.line(),
                                       loc.function_name()));
    Logger::instance().flush();
    std::abort();
}
} // namespace lumora::detail

/// Checked in all build configurations: use for invariants whose violation would corrupt state.
#define LUMORA_VERIFY(cond, msg)                                                                  \
    do {                                                                                          \
        if (!(cond)) [[unlikely]] {                                                               \
            ::lumora::detail::assertionFailed(#cond, (msg), std::source_location::current());     \
        }                                                                                         \
    } while (false)

#if defined(LUMORA_DEBUG)
#define LUMORA_ASSERT(cond, msg) LUMORA_VERIFY(cond, msg)
#else
#define LUMORA_ASSERT(cond, msg) ((void)0)
#endif
