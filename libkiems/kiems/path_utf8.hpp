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

/// The folder a simulation's output goes in, inside each stage's directory (ems/geometry,
/// ems/simulation, ems/results): the simulation's name, as a path.
///
/// On Windows, a name that isn't a valid file name is escaped -- each of < > : " / \ | ? *, control
/// characters, and the escape character % itself, as %XX; trailing dots and spaces, which Windows
/// drops from a file name (and keeps in a folder further up a path, making it unreachable); and the
/// first letter of a reserved device name (CON, NUL, COM1...). Names built from KiCad's
/// hierarchical net names, "/sheet/NET", need this: a Simulation Set names its simulations
/// "<set> <net>". Elsewhere the name is used as it is (a '/' nests folders), as it always has been,
/// so existing packages keep their layout.
inline std::filesystem::path simulationDirectoryName(std::string_view name) {
#ifdef _WIN32
    std::string out;
    const auto escape = [&out](unsigned char c) {
        constexpr char hex[] = "0123456789ABCDEF";
        out += '%';
        out += hex[c >> 4];
        out += hex[c & 15];
    };
    constexpr std::string_view forbidden = "<>:\"/\\|?*%";
    std::size_t end = name.size();
    while (end > 0 && (name[end - 1] == ' ' || name[end - 1] == '.')) --end;
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto c = static_cast<unsigned char>(name[i]);
        if (c < 0x20 || i >= end || forbidden.find(static_cast<char>(c)) != std::string_view::npos) escape(c);
        else out += static_cast<char>(c);
    }
    // CON, PRN, AUX, NUL, COM1-9 and LPT1-9 name devices, with any extension and in any case.
    std::string stem = out.substr(0, out.find('.'));
    for (char& c : stem) c = (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
    const bool numbered = stem.size() == 4 && (stem.starts_with("COM") || stem.starts_with("LPT")) &&
                          stem[3] >= '1' && stem[3] <= '9';
    if (numbered || stem == "CON" || stem == "PRN" || stem == "AUX" || stem == "NUL") {
        const auto first = static_cast<unsigned char>(out[0]);
        out.erase(0, 1);
        const std::string rest = std::move(out);
        out.clear();
        escape(first);
        out += rest;
    }
    return pathFromUtf8(out);
#else
    return pathFromUtf8(name);
#endif
}

/// The simulation name simulationDirectoryName() made `directory` from.
inline std::string simulationNameFromDirectory(const std::filesystem::path& directory) {
    std::string name = pathToUtf8(directory);
#ifdef _WIN32
    const auto digit = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    std::string out;
    for (std::size_t i = 0; i < name.size(); ++i) {
        if (name[i] == '%' && i + 2 < name.size() && digit(name[i + 1]) >= 0 && digit(name[i + 2]) >= 0) {
            out += static_cast<char>(digit(name[i + 1]) * 16 + digit(name[i + 2]));
            i += 2;
        } else {
            out += name[i];
        }
    }
    name = std::move(out);
#endif
    return name;
}

} // namespace kiems
