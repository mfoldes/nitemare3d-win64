#include "config_storage.hpp"

#include "n3d/binary_reader.hpp"

#include <Windows.h>

#include <array>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <system_error>

namespace n3d::platform {
namespace {

[[nodiscard]] std::filesystem::path config_path_for(const std::filesystem::path& user_directory) {
    return user_directory / "config.sav";
}

} // namespace

Config load_user_config(const std::filesystem::path& user_directory) {
    const auto path = config_path_for(user_directory);

    std::error_code inspect_error;
    const bool present = std::filesystem::exists(path, inspect_error);
    if (inspect_error) {
        throw std::filesystem::filesystem_error("Cannot inspect user config.sav", path, inspect_error);
    }
    if (!present) {
        return Config{};
    }

    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        throw DataError(path.string(), 0, "config.sav file can be opened");
    }

    const auto file_size = stream.tellg();
    if (file_size < 0) {
        throw DataError(path.string(), 0, "config.sav file size can be read");
    }
    if (static_cast<std::size_t>(file_size) != Config::kSerializedSize) {
        throw DataError(path.string(), static_cast<std::size_t>(file_size),
                        "config.sav payload is exactly 20 bytes");
    }

    std::array<std::byte, Config::kSerializedSize> bytes{};
    stream.seekg(0, std::ios::beg);
    if (!stream.read(reinterpret_cast<char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()))) {
        throw DataError(path.string(), 0, "config.sav file contents can be read");
    }

    const auto config = Config::decode(bytes, path.string());
    config.validate(path.string(), 0);
    return config;
}

void save_user_config(const std::filesystem::path& user_directory, const Config& config) {
    const auto target = config_path_for(user_directory);

    std::error_code directory_error;
    std::filesystem::create_directories(user_directory, directory_error);
    if (directory_error) {
        throw std::filesystem::filesystem_error(
            "Cannot create user config directory", user_directory, directory_error);
    }

    config.validate(target.string(), 0);

    const auto payload = config.encode();
    std::filesystem::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;

    try {
        for (unsigned attempt = 0; attempt < 32; ++attempt) {
            temporary = target;
            temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                         std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
            file = CreateFileW(temporary.c_str(), GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                               nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                break;
            }
            if (GetLastError() != ERROR_FILE_EXISTS) {
                throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                        "Create user config.sav temporary file");
            }
        }
        if (file == INVALID_HANDLE_VALUE) {
            throw std::runtime_error("Cannot reserve a user config.sav temporary file");
        }

        DWORD written = 0;
        if (!WriteFile(file, payload.data(), static_cast<DWORD>(payload.size()), &written, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Write user config.sav");
        }
        if (written != payload.size()) {
            throw std::runtime_error("User config.sav was only partially written");
        }
        if (!FlushFileBuffers(file)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Flush user config.sav");
        }
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Close user config.sav");
        }
        file = INVALID_HANDLE_VALUE;

        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Replace user config.sav");
        }
        temporary.clear();
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) {
            CloseHandle(file);
            file = INVALID_HANDLE_VALUE;
        }
        if (!temporary.empty()) {
            std::error_code remove_error;
            std::filesystem::remove(temporary, remove_error);
        }
        throw;
    }
}

} // namespace n3d::platform
