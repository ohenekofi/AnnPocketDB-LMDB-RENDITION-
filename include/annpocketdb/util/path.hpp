#pragma once

// Path helpers that never narrow a std::filesystem::path through the ANSI
// code page. On Windows, path::string() converts via the active code page:
// characters it can't represent are lost (so a rebuilt sidecar path names a
// different file) or libstdc++ throws std::system_error. Every filesystem
// path inside the engine therefore stays a std::filesystem::path down to the
// OS call; only human-readable text (Status messages) goes through
// pathToUtf8().

#include <filesystem>
#include <string>
#include <string_view>

namespace annpocketdb::util {

// dataPath + suffix (e.g. ".wal", ".lock", ".sha256", ".enc"). path::operator+=
// appends to the native string, so no narrowing happens on any platform.
inline std::filesystem::path withSuffix(std::filesystem::path p, std::string_view suffix) {
    p += suffix;
    return p;
}

// UTF-8 text for error messages / logs. u8string() converts from the native
// UTF-16 on Windows (every Unicode character is representable) and is the
// identity on POSIX. The only remaining failure is ill-formed UTF-16 (a lone
// surrogate in a Windows file name); message text must never throw, so that
// case degrades to a placeholder.
inline std::string pathToUtf8(const std::filesystem::path& p) noexcept {
    try {
        const auto u8 = p.u8string();
        return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
    } catch (...) {
        return "<path not representable as UTF-8>";
    }
}

// Inverse of pathToUtf8(): interpret a narrow string as UTF-8 regardless of
// the platform's narrow encoding. Used at the C API / CLI boundary. On
// Windows, ill-formed UTF-8 throws std::system_error / filesystem_error, so
// callers at an API boundary must catch. On POSIX the bytes are taken as-is
// (exactly what path(std::string) did before), so names that aren't valid
// UTF-8 keep working there.
inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
#if defined(_WIN32)
    return std::filesystem::path(
        std::u8string(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
#else
    return std::filesystem::path(std::string(utf8));
#endif
}

}  // namespace annpocketdb::util
