// Windows-only support for the kiems CLI: locating its own executable, finding gnuplot, and
// reporting an uncaught exception. It lives in its own translation unit -- compiled only on
// Windows (see the root CMakeLists.txt) -- so <windows.h>'s macros never reach main.cpp's libkiems
// and CopperUtils headers.

#if defined(_WIN32)

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <typeinfo>

namespace kiems_cli {

void reportUncaughtExceptions() {
    // macOS's and Linux's C++ runtimes print an uncaught exception's type and message before
    // aborting; MSVC's runtime aborts silently. Say what happened, as they do.
    std::set_terminate([] {
        if (const std::exception_ptr exception = std::current_exception()) {
            try {
                std::rethrow_exception(exception);
            } catch (const std::exception& error) {
                std::fprintf(stderr, "terminating due to uncaught exception of type %s: %s\n", typeid(error).name(),
                             error.what());
            } catch (...) {
                std::fprintf(stderr, "terminating due to uncaught exception of unknown type\n");
            }
        } else {
            std::fprintf(stderr, "terminating: std::terminate called without an active exception\n");
        }
        std::fflush(stderr);
        std::abort();
    });
}

std::filesystem::path currentExecutablePath() {
    // GetModuleFileNameW truncates (and returns the buffer size) when the buffer is too small;
    // long-path-aware processes can exceed MAX_PATH, so grow until the whole path fits.
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            return {};
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return std::filesystem::path(buffer);
        }
        if (buffer.size() >= 32768) { // The longest path Windows supports.
            return {};
        }
        buffer.resize(buffer.size() * 2);
    }
}

void addGnuplotToPathIfMissing() {
    // Matplot++ runs "gnuplot" by name. gnuplot's Windows installer (also what winget runs) leaves
    // PATH alone by default, so look in its standard location before plotting fails.
    std::array<wchar_t, MAX_PATH> found{};
    if (::SearchPathW(nullptr, L"gnuplot.exe", nullptr, static_cast<DWORD>(found.size()), found.data(), nullptr) != 0) {
        return;
    }
    for (const wchar_t* variable : {L"ProgramFiles", L"ProgramFiles(x86)"}) {
        const wchar_t* root = _wgetenv(variable);
        if (root == nullptr) continue;
        const std::filesystem::path bin = std::filesystem::path(root) / L"gnuplot" / L"bin";
        std::error_code error;
        if (!std::filesystem::is_regular_file(bin / L"gnuplot.exe", error)) continue;
        const wchar_t* path = _wgetenv(L"PATH");
        const std::wstring updated = (path != nullptr && path[0] != L'\0' ? std::wstring(path) + L";" : std::wstring()) +
                                     bin.wstring();
        // _wputenv_s updates both the C runtime's environment (which _popen passes on) and the process's.
        _wputenv_s(L"PATH", updated.c_str());
        return;
    }
}

} // namespace kiems_cli

#endif
