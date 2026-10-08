#include "save_storage.hpp"

#include "n3d/binary_reader.hpp"

#include <Windows.h>

#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <system_error>
#include <vector>

namespace n3d::platform {
namespace {

constexpr std::size_t kNativeSaveHeaderSize = 56u;
constexpr std::size_t kNativeSaveMinimumPayload = 79922u;
constexpr std::size_t kNativeSaveMaximumPayload = 112924u;
constexpr std::size_t kNativeSaveMinimumSize = kNativeSaveHeaderSize + kNativeSaveMinimumPayload;
constexpr std::size_t kNativeSaveMaximumSize = kNativeSaveHeaderSize + kNativeSaveMaximumPayload;
[[nodiscard]] std::wstring slot_filename(std::size_t slot) {
    if (slot >= 10u) {
        throw std::out_of_range("native save slot index must be 0..9");
    }
    const auto slot_number = slot + 1u;
    std::wstring name = L"slot-";
    if (slot_number < 10u) {
        name += L"0";
    }
    name += std::to_wstring(slot_number);
    name += L".n3dsave";
    return name;
}

[[nodiscard]] std::filesystem::path save_directory_for(const std::filesystem::path& user_directory) {
    return user_directory / L"saves";
}

void throw_system_error(const char* what, DWORD error_code) {
    throw std::system_error(static_cast<int>(error_code), std::system_category(), what);
}

[[nodiscard]] HANDLE create_unique_temp_slot(const std::filesystem::path& save_directory,
                                            std::size_t slot,
                                            std::filesystem::path& temp_path) {
    const auto name_root = slot_filename(slot);
    for (unsigned attempt = 0; attempt < 256u; ++attempt) {
        const auto unique = std::wstring(L".") + std::to_wstring(GetCurrentProcessId()) +
                            L"." + std::to_wstring(GetTickCount64()) + L"." +
                            std::to_wstring(attempt);
        temp_path = save_directory / (name_root + unique + L".tmp");
        const HANDLE handle = CreateFileW(temp_path.native().c_str(), GENERIC_WRITE,
                                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                         nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                         nullptr);
        if (handle != INVALID_HANDLE_VALUE) {
            return handle;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_EXISTS && error != ERROR_ALREADY_EXISTS) {
            throw_system_error("CreateFileW unique native save temp file", error);
        }
    }
    throw std::runtime_error("Could not reserve a unique native save temporary file");
}

} // namespace
std::filesystem::path native_slot_path(const std::filesystem::path& user_directory,
                                      std::size_t slot) {
    return save_directory_for(user_directory) / slot_filename(slot);
}

bool native_slot_present(const std::filesystem::path& user_directory, std::size_t slot) {
    const auto path = native_slot_path(user_directory, slot);
    std::error_code inspect_error;
    if (!std::filesystem::exists(path, inspect_error)) {
        if (inspect_error) {
            throw std::filesystem::filesystem_error("Cannot inspect native save slot", path,
                                                   inspect_error);
        }
        return false;
    }

    if (!std::filesystem::is_regular_file(path, inspect_error)) {
        if (inspect_error) {
            throw std::filesystem::filesystem_error("Cannot inspect native save slot", path,
                                                   inspect_error);
        }
        throw DataError(path.string(), 0, "native save slot path is a regular file");
    }
    return true;
}

std::optional<std::vector<std::byte>> load_native_slot(const std::filesystem::path& user_directory,
                                                      std::size_t slot) {
    const auto path = native_slot_path(user_directory, slot);

    std::error_code inspect_error;
    if (!std::filesystem::exists(path, inspect_error)) {
        if (inspect_error) {
            throw std::filesystem::filesystem_error("Cannot inspect native save slot", path,
                                                   inspect_error);
        }
        return std::nullopt;
    }

    if (!std::filesystem::is_regular_file(path, inspect_error)) {
        if (inspect_error) {
            throw std::filesystem::filesystem_error("Cannot inspect native save slot", path,
                                                   inspect_error);
        }
        throw DataError(path.string(), 0, "native save slot path is a regular file");
    }

    const auto file_size = std::filesystem::file_size(path, inspect_error);
    if (inspect_error) {
        throw std::filesystem::filesystem_error("Cannot read native save size", path, inspect_error);
    }
    if (file_size < static_cast<std::uintmax_t>(kNativeSaveMinimumSize) ||
        file_size > static_cast<std::uintmax_t>(kNativeSaveMaximumSize)) {
        throw DataError(path.string(), static_cast<std::size_t>(file_size),
                        "native save file size is within the frozen codec bounds");
    }

    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw DataError(path.string(), 0, "native save file can be opened");
    }

    std::vector<std::byte> bytes(static_cast<std::size_t>(file_size));
    stream.read(reinterpret_cast<char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    if (!stream) {
        throw DataError(path.string(), static_cast<std::size_t>(stream.gcount()),
                        "native save file contents can be read completely");
    }
    return bytes;
}

void save_native_slot(const std::filesystem::path& user_directory, std::size_t slot,
                      std::span<const std::byte> encoded) {
    if (encoded.size() < kNativeSaveMinimumSize || encoded.size() > kNativeSaveMaximumSize) {
        throw std::length_error("native save payload size is within the frozen codec bounds");
    }

    const auto path = native_slot_path(user_directory, slot);
    const auto save_directory = save_directory_for(user_directory);

    std::error_code directory_error;
    std::filesystem::create_directories(save_directory, directory_error);
    if (directory_error) {
        throw std::filesystem::filesystem_error("Cannot create native save directory", save_directory,
                                               directory_error);
    }

    std::filesystem::path temp_path;
    const HANDLE temp_handle = create_unique_temp_slot(save_directory, slot, temp_path);

    try {
        DWORD bytes_written = 0;
        const BOOL wrote = WriteFile(temp_handle, encoded.data(),
                                     static_cast<DWORD>(encoded.size()), &bytes_written, nullptr);
        if (!wrote || static_cast<std::size_t>(bytes_written) != encoded.size()) {
            const DWORD error = GetLastError();
            CloseHandle(temp_handle);
            DeleteFileW(temp_path.c_str());
            if (!wrote) throw_system_error("WriteFile native save temp file", error);
            throw std::runtime_error("Native save temporary file was only partially written");
        }

        if (!FlushFileBuffers(temp_handle)) {
            const DWORD error = GetLastError();
            CloseHandle(temp_handle);
            DeleteFileW(temp_path.c_str());
            throw_system_error("FlushFileBuffers native save temp file", error);
        }

        if (!CloseHandle(temp_handle)) {
            const DWORD error = GetLastError();
            DeleteFileW(temp_path.c_str());
            throw_system_error("CloseHandle native save temp file", error);
        }

        if (!MoveFileExW(temp_path.native().c_str(), path.native().c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const DWORD error = GetLastError();
            DeleteFileW(temp_path.c_str());
            throw_system_error("MoveFileExW native save temp file", error);
        }
    } catch (...) {
        DeleteFileW(temp_path.c_str());
        throw;
    }
}

} // namespace n3d::platform
