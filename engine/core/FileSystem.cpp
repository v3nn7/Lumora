#include "core/FileSystem.hpp"

#include <fstream>
#include <system_error>

namespace lumora::fs {

namespace {
template <class Container>
Result<Container> readWhole(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        return makeError("cannot open '{}' for reading", path.string());
    }
    const std::streamsize size = file.tellg();
    if (size < 0) {
        return makeError("cannot determine size of '{}'", path.string());
    }
    Container data(static_cast<std::size_t>(size), typename Container::value_type{});
    file.seekg(0);
    if (size > 0 && !file.read(reinterpret_cast<char*>(data.data()), size)) {
        return makeError("failed reading '{}'", path.string());
    }
    return data;
}
} // namespace

Result<std::string> readTextFile(const std::filesystem::path& path) { return readWhole<std::string>(path); }

Result<std::vector<std::byte>> readBinaryFile(const std::filesystem::path& path) {
    return readWhole<std::vector<std::byte>>(path);
}

Result<> writeFileAtomic(const std::filesystem::path& path, std::span<const std::byte> data) {
    std::error_code ec;
    if (path.has_parent_path()) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (ec) {
            return makeError("cannot create directory '{}': {}", path.parent_path().string(), ec.message());
        }
    }
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream file(tmp, std::ios::binary | std::ios::trunc);
        if (!file) {
            return makeError("cannot open '{}' for writing", tmp.string());
        }
        file.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!file.flush()) {
            return makeError("failed writing '{}'", tmp.string());
        }
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return makeError("cannot replace '{}'", path.string());
    }
    return {};
}

Result<> writeTextFileAtomic(const std::filesystem::path& path, std::string_view text) {
    return writeFileAtomic(path, std::as_bytes(std::span(text.data(), text.size())));
}

} // namespace lumora::fs
