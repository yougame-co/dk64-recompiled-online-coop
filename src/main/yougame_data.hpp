#ifndef YG_YOUGAME_DATA_HPP
#define YG_YOUGAME_DATA_HPP

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <stdexcept>
#include <cstring>
#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif

// The value after `--rom`, or empty. Windows reads the wide command line so a path with non-ASCII letters survives.
inline std::filesystem::path yg_rom_argument(int argc, char **argv) {
#ifdef _WIN32
    (void)argc; (void)argv;
    std::filesystem::path found;
    int count = 0;
    LPWSTR *wide = CommandLineToArgvW(GetCommandLineW(), &count);
    if (wide) {
        for (int i = 1; i + 1 < count; i++) {
            if (wcscmp(wide[i], L"--rom") == 0) { found = std::filesystem::path(wide[i + 1]); break; }
        }
        LocalFree(wide);
    }
    return found;
#else
    for (int i = 1; i + 1 < argc; i++) {
        if (std::strcmp(argv[i], "--rom") == 0) return std::filesystem::path(argv[i + 1]);
    }
    return {};
#endif
}

// Before any frontend/config initialization. The portable marker in our data
// cwd also outranks an old marker beside the read-only .app (frontend file.cpp).
// Standalone launches keep the frontend's existing portable/env/support rules.
inline bool yg_use_app_data() {
    const char *stdio = std::getenv("YOUGAME_STDIO");
#ifdef _WIN32
    const wchar_t *data = _wgetenv(L"YOUGAME_DATA");
#else
    const char *data = std::getenv("YOUGAME_DATA");
#endif
    if (!stdio || std::string(stdio) != "1" || !data || !*data) return true;
    try {
        const std::filesystem::path dir(data);
        if (!dir.is_absolute()) throw std::runtime_error("relative data path");
        std::filesystem::create_directories(dir);
        std::filesystem::current_path(dir);
        const auto marker = dir / "portable.txt";
        if (!std::filesystem::exists(marker)) {
            std::ofstream file(marker, std::ios::out);
            file.close();
            if (!file) throw std::runtime_error("portable marker");
        }
#ifdef _WIN32
        if (_wputenv_s(L"APP_FOLDER_PATH", data)) throw std::runtime_error("data environment");
#else
        // getenv storage may be invalidated by setenv, so copy the value first.
        const std::string folder(data);
        if (::setenv("APP_FOLDER_PATH", folder.c_str(), 1)) throw std::runtime_error("data environment");
#endif
        return true;
    } catch (const std::exception &) {
        std::fprintf(stderr, "YG-COOP error app-data-folder\n");
        return false;
    }
}
#endif
