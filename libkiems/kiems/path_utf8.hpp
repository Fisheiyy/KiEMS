// UTF-8 conversions for std::filesystem::path wherever a path crosses a narrow-string boundary
// (JSON files, job files, log and error text, libkicad's std::string API).
//
// On macOS and Linux a path's native narrow form already is its UTF-8 byte sequence, so these are
// identity conversions there. On Windows, path::string() and path(std::string) use the active ANSI
// code page instead, which loses -- or throws on -- characters that code page cannot represent, and
// would make files written on one machine unreadable on another.
#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace kiems {

inline std::string pathToUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.u8string();
    return std::string(utf8.begin(), utf8.end());
}

inline std::filesystem::path pathFromUtf8(std::string_view utf8) {
    return std::filesystem::path(std::u8string(utf8.begin(), utf8.end()));
}

/// The portable, forward-slash form of a path as UTF-8. Use it for paths persisted in project files
/// so a project written on Windows still opens on macOS and Linux (Windows accepts '/' too).
inline std::string pathToGenericUtf8(const std::filesystem::path& path) {
    const std::u8string utf8 = path.generic_u8string();
    return std::string(utf8.begin(), utf8.end());
}

} // namespace kiems
