#include <Windows.h>
#include "n3d/binary_reader.hpp"

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "../src/platform/save_storage.hpp"

namespace {

int failures = 0;

void check(bool condition, std::string_view label) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << label << '\n';
    }
}

class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        const auto tick = GetTickCount64();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto candidate = base / (L"n3d_save_test_" + std::to_wstring(GetCurrentProcessId()) +
                                     L"." + std::to_wstring(tick) + L"." +
                                     std::to_wstring(attempt));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot create a unique native save test directory");
    }

    ~ScopedTempDirectory() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDirectory(const ScopedTempDirectory&) = delete;
    ScopedTempDirectory& operator=(const ScopedTempDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

[[nodiscard]] std::vector<std::byte> bounded_slot_bytes() {
    // Filesystem storage is format-agnostic; encode_save/decode_save own the
    // independent header, identity and CRC behavior tests.
    return std::vector<std::byte>(80000u, std::byte{0xA5});
}

[[nodiscard]] std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) {
        return {};
    }
    const auto size = static_cast<std::size_t>(stream.tellg());
    stream.seekg(0, std::ios::beg);
    std::vector<std::byte> bytes(size);
    if (size != 0) {
        stream.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    }
    return bytes;
}

[[nodiscard]] bool has_temp_residue(const std::filesystem::path& dir) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().wstring().find(L".tmp") != std::wstring::npos) {
            return true;
        }
    }
    return false;
}

template <typename Fn>
void expect_data_error(Fn&& fn, const std::filesystem::path& path, std::size_t offset,
                      std::string_view label) {
    try {
        fn();
        check(false, std::string(label) + " is rejected");
    } catch (const n3d::DataError& error) {
        check(error.path() == path.string(), std::string(label) + " names the native slot file");
        check(error.offset() == offset, std::string(label) + " reports the expected offset");
    }
}


void invalid_write_leaves_previous_good_slot_unchanged() {
    const ScopedTempDirectory dir;
    const auto slot_path = n3d::platform::native_slot_path(dir.path(), 0);
    const auto before = bounded_slot_bytes();
    n3d::platform::save_native_slot(dir.path(), 0, before);
    const auto original = read_bytes(slot_path);

    try {
        n3d::platform::save_native_slot(dir.path(), 0, std::vector<std::byte>{std::byte{1}, std::byte{2}});
        check(false, "invalid native save payload size is rejected");
    } catch (const std::length_error&) {
    }

    check(read_bytes(slot_path) == original, "rejected native save leaves the old slot file byte-identical");
    check(!has_temp_residue(dir.path() / L"saves"), "rejected native save leaves no temp files");
}

void sharing_denied_replace_preserves_previous_slot() {
    const ScopedTempDirectory dir;
    const auto slot_path = n3d::platform::native_slot_path(dir.path(), 0);
    const auto before = bounded_slot_bytes();
    n3d::platform::save_native_slot(dir.path(), 0, before);
    const HANDLE locked = CreateFileW(slot_path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                                      nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (locked == INVALID_HANDLE_VALUE)
        throw std::runtime_error("Cannot lock native save slot for atomic replacement test");
    auto replacement = bounded_slot_bytes();
    replacement[128] = std::byte{0x42};
    bool rejected = false;
    try {
        n3d::platform::save_native_slot(dir.path(), 0, replacement);
    } catch (const std::system_error&) {
        rejected = true;
    }
    CloseHandle(locked);
    check(rejected, "sharing denial prevents replacing an existing native slot");
    check(read_bytes(slot_path) == before, "failed final replace preserves the previous save bytes");
    check(!has_temp_residue(dir.path() / L"saves"), "failed final replace removes only its own temp");
}

void invalid_or_truncated_file_reads_throw_data_error() {
    const ScopedTempDirectory dir;
    const auto path = dir.path() / L"saves" / L"slot-01.n3dsave";
    std::filesystem::create_directories(path.parent_path());

    std::vector<std::byte> too_small(79969u, std::byte{0x55});
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(too_small.data()),
                 static_cast<std::streamsize>(too_small.size()));
    stream.close();

    expect_data_error([&] { (void)n3d::platform::load_native_slot(dir.path(), 0); }, path, 79969u,
                      "truncated native save read");

    const auto nonregular = dir.path() / L"saves" / L"slot-02.n3dsave";
    std::filesystem::create_directory(nonregular);
    try {
        (void)n3d::platform::native_slot_present(dir.path(), 1);
        check(false, "nonregular native save path is rejected");
    } catch (const n3d::DataError&) {
    }
}

} // namespace

int run_save_storage_tests() {
    invalid_write_leaves_previous_good_slot_unchanged();
    sharing_denied_replace_preserves_previous_slot();
    invalid_or_truncated_file_reads_throw_data_error();

    if (failures != 0) {
        std::cerr << "n3d native save storage checks failed: " << failures << " checks\n";
        return failures;
    }
    std::cout << "n3d native save storage checks passed\n";
    return 0;
}
