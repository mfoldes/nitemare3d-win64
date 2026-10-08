#include "../src/platform/config_storage.hpp"

#include "n3d/binary_reader.hpp"

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, std::string_view message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}

// Unique per-test directory under the OS temp root, removed on scope exit;
// pid + boot tick + attempt counter re-probed by create_directories, no PRNG.
class ScopedTempDirectory {
public:
    ScopedTempDirectory() {
        const auto base = std::filesystem::temp_directory_path();
        const auto tick = GetTickCount64();
        for (unsigned attempt = 0; attempt < 64; ++attempt) {
            auto candidate = base / (L"n3d_cfg_test_" + std::to_wstring(GetCurrentProcessId()) +
                                     L"." + std::to_wstring(tick) + L"." + std::to_wstring(attempt));
            std::error_code ec;
            if (std::filesystem::create_directories(candidate, ec) && !ec) {
                path_ = std::move(candidate);
                return;
            }
        }
        throw std::runtime_error("Cannot create a unique config storage test directory");
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

[[nodiscard]] std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

[[nodiscard]] bool has_temp_residue(const std::filesystem::path& dir) {
    for (const auto& entry : std::filesystem::directory_iterator(dir)) {
        if (entry.path().filename().wstring().find(L".tmp.") != std::wstring::npos) return true;
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
        check(error.path() == path.string(), std::string(label) + " names config.sav");
        check(error.offset() == offset, std::string(label) + " reports the expected offset");
    }
}

void missing_file_loads_defaults_without_creating_config_sav() {
    const ScopedTempDirectory dir;

    const auto loaded = n3d::platform::load_user_config(dir.path());
    check(loaded.viewport_width == n3d::Config::kDefaultViewportWidth, "missing file loads the default 304-column viewport");
    check(loaded.music_volume == n3d::Config::kDefaultMusicVolume, "missing file loads the default music volume 60");
    check(!std::filesystem::exists(dir.path() / "config.sav"), "loading a missing config.sav does not create the file");
    check(!has_temp_residue(dir.path()), "loading a missing config.sav leaves no temp files");
}

void save_then_reload_roundtrips_modified_fields_without_temp_residue() {
    const ScopedTempDirectory dir;
    const auto saved = dir.path() / "config.sav";

    n3d::Config modified{};
    modified.viewport_width = 256u;
    modified.unknown_bytes = {0xABu, 0xCDu};
    modified.output_width = 640u;
    modified.output_height = 400u;
    modified.music_volume = 82u;
    n3d::platform::save_user_config(dir.path(), modified);

    check(std::filesystem::file_size(saved) == n3d::Config::kSerializedSize, "saved config.sav is exactly 20 bytes");
    check(!has_temp_residue(dir.path()), "a valid save leaves no temp files");

    const auto loaded = n3d::platform::load_user_config(dir.path());
    check(loaded.viewport_width == 256u, "modified viewport width survives save/load");
    check(loaded.unknown_bytes[0] == 0xABu && loaded.unknown_bytes[1] == 0xCDu,
          "opaque +2/+3 bytes survive save/load unnormalized");
    check(loaded.music_volume == 82u, "modified music volume survives save/load");
}

void invalid_save_throws_and_keeps_previous_file() {
    const ScopedTempDirectory dir;
    const auto saved = dir.path() / "config.sav";

    n3d::Config good{};
    good.music_volume = 40u;
    n3d::platform::save_user_config(dir.path(), good);
    const auto previous = read_bytes(saved);

    n3d::Config invalid{};
    invalid.joystick_enabled = 1u; // zero divisor with the joystick enabled is invalid
    invalid.joystick_divisor = 0u;
    expect_data_error([&] { n3d::platform::save_user_config(dir.path(), invalid); },
                      saved, 9u, "an invalid joystick divisor save");
    check(read_bytes(saved) == previous, "a rejected save leaves the previous file byte-identical");
    check(!has_temp_residue(dir.path()), "a rejected save leaves no temp files");
}

void wrong_length_file_throws_instead_of_defaulting() {
    const ScopedTempDirectory dir;
    const auto saved = dir.path() / "config.sav";
    {
        const std::string truncated(15, '\0'); // content irrelevant; the size check fires first
        std::ofstream stream(saved, std::ios::binary | std::ios::trunc);
        stream.write(truncated.data(), static_cast<std::streamsize>(truncated.size()));
    }
    expect_data_error([&] { static_cast<void>(n3d::platform::load_user_config(dir.path())); },
                      saved, 15u, "a 15-byte config.sav");
}

} // namespace

int run_config_storage_tests() {
    missing_file_loads_defaults_without_creating_config_sav();
    save_then_reload_roundtrips_modified_fields_without_temp_residue();
    invalid_save_throws_and_keeps_previous_file();
    wrong_length_file_throws_instead_of_defaulting();

    if (failures != 0) {
        std::cerr << "n3d config storage checks failed: " << failures << " checks\n";
        return failures;
    }
    std::cout << "n3d config storage checks passed\n";
    return 0;
}
