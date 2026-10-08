#include "paths.hpp"

#include <shlobj.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")
#pragma comment(lib, "Uuid.lib")

namespace n3d::platform {
namespace {

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;
constexpr std::size_t kMaxSettingsBytes = 1024 * 1024;

void check_hresult(HRESULT result, const char* operation) {
    if (FAILED(result)) {
        throw std::system_error(static_cast<int>(result), std::system_category(), operation);
    }
}

[[nodiscard]] fs::path absolute_once(const fs::path& path) {
    return fs::absolute(path).lexically_normal();
}

[[nodiscard]] fs::path executable_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                                 static_cast<DWORD>(buffer.size()));
        if (length == 0) {
            throw std::system_error(static_cast<int>(GetLastError()),
                                    std::system_category(), "GetModuleFileNameW");
        }
        if (length < buffer.size()) {
            buffer.resize(length);
            return fs::path(buffer).parent_path();
        }
        if (buffer.size() >= 32768) {
            throw std::runtime_error("Executable path exceeds the Windows path limit");
        }
        buffer.resize(std::min<std::size_t>(buffer.size() * 2, 32768));
    }
}

[[nodiscard]] fs::path default_user_directory() {
    PWSTR raw = nullptr;
    check_hresult(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT,
                                       nullptr, &raw), "Locate LOCALAPPDATA");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    return absolute_once(fs::path(owned.get()) / L"Nitemare3D");
}

[[nodiscard]] std::wstring decode_utf8(std::string_view bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("settings.ini is too large to decode");
    }
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                                         static_cast<int>(bytes.size()), nullptr, 0);
    if (!size) throw std::runtime_error("settings.ini contains invalid UTF-8");
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(),
                             static_cast<int>(bytes.size()), result.data(), size)) {
        throw std::runtime_error("settings.ini contains invalid UTF-8");
    }
    return result;
}
[[nodiscard]] std::wstring decode_diagnostic(std::string_view bytes) {
    if (bytes.empty()) return {};
    const auto length = static_cast<int>(bytes.size());
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int size = MultiByteToWideChar(code_page, flags, bytes.data(), length, nullptr, 0);
    if (!size) {
        // DataError paths originating in std::filesystem::path::string() can
        // carry the active Windows code page rather than UTF-8.
        code_page = CP_ACP;
        flags = 0;
        size = MultiByteToWideChar(code_page, flags, bytes.data(), length, nullptr, 0);
    }
    if (!size) return L"(unprintable asset path)";
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (!MultiByteToWideChar(code_page, flags, bytes.data(), length, result.data(), size)) {
        return L"(unprintable asset path)";
    }
    return result;
}


[[nodiscard]] std::string encode_utf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
        throw std::runtime_error("settings.ini is too large to encode");
    }
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size) throw std::runtime_error("A settings path contains invalid Unicode");
    std::string result(static_cast<std::size_t>(size), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                             static_cast<int>(text.size()), result.data(), size, nullptr, nullptr)) {
        throw std::runtime_error("A settings path contains invalid Unicode");
    }
    return result;
}

[[nodiscard]] std::wstring read_settings(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary | std::ios::ate);
    if (!stream) throw std::runtime_error("Cannot open user settings.ini");
    const auto length = stream.tellg();
    if (length < 0 || static_cast<std::uintmax_t>(length) > kMaxSettingsBytes) {
        throw std::runtime_error("User settings.ini exceeds the size limit");
    }
    std::string bytes(static_cast<std::size_t>(length), '\0');
    stream.seekg(0, std::ios::beg);
    if (!bytes.empty() && !stream.read(bytes.data(), static_cast<std::streamsize>(bytes.size()))) {
        throw std::runtime_error("Cannot read user settings.ini");
    }
    if (bytes.starts_with("\xef\xbb\xbf")) bytes.erase(0, 3);
    auto text = decode_utf8(bytes);
    if (text.find(L'\0') != std::wstring::npos) {
        throw std::runtime_error("User settings.ini contains a NUL character");
    }
    return text;
}

[[nodiscard]] bool same_name(std::wstring_view left, std::wstring_view right) {
    return CompareStringOrdinal(left.data(), static_cast<int>(left.size()),
                                right.data(), static_cast<int>(right.size()), TRUE) == CSTR_EQUAL;
}

[[nodiscard]] std::wstring_view trim_ascii(std::wstring_view text) {
    const auto first = text.find_first_not_of(L" \t");
    if (first == std::wstring_view::npos) return {};
    return text.substr(first, text.find_last_not_of(L" \t") - first + 1);
}

struct SettingLocation final {
    std::optional<std::wstring> value;
    std::size_t value_begin = 0;
    std::size_t value_end = 0;
    std::optional<std::size_t> section_end;
};

[[nodiscard]] SettingLocation locate_data_path(const std::wstring& text) {
    SettingLocation found;
    bool in_paths = false;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of(L"\r\n", start);
        const auto line_end = end == std::wstring::npos ? text.size() : end;
        std::size_t next = line_end;
        if (next < text.size() && text[next] == L'\r') ++next;
        if (next < text.size() && text[next] == L'\n') ++next;
        const auto line = trim_ascii(std::wstring_view(text).substr(start, line_end - start));
        if (line.size() >= 2 && line.front() == L'[' && line.back() == L']') {
            in_paths = same_name(trim_ascii(line.substr(1, line.size() - 2)), L"Paths");
            if (in_paths && !found.section_end) found.section_end = next;
        } else if (in_paths && !line.empty() && line.front() != L';' && line.front() != L'#') {
            const auto equal = line.find(L'=');
            if (equal != std::wstring_view::npos &&
                same_name(trim_ascii(line.substr(0, equal)), L"data_path")) {
                if (found.value) throw std::runtime_error("settings.ini has duplicate Paths/data_path entries");
                const auto value = trim_ascii(line.substr(equal + 1));
                found.value = std::wstring(value);
                const auto line_offset = static_cast<std::size_t>(line.data() - text.data());
                found.value_begin = line_offset + equal + 1;
                found.value_end = line_end;
            }
        }
        start = next;
    }
    return found;
}

[[nodiscard]] std::optional<fs::path> saved_data_directory(const fs::path& settings) {
    std::error_code error;
    const bool present = fs::exists(settings, error);
    if (error) throw fs::filesystem_error("Cannot inspect user settings.ini", settings, error);
    if (!present) return std::nullopt;
    const auto location = locate_data_path(read_settings(settings));
    if (!location.value) return std::nullopt;
    return fs::path(*location.value);
}

// Only this file is replaced. The temporary file is in the same user-owned
// directory, flushed before rename; neither asset files nor their directory are touched.
void save_data_directory(const fs::path& settings, const fs::path& selected) {
    std::wstring text;
    std::error_code error;
    const bool present = fs::exists(settings, error);
    if (error) throw fs::filesystem_error("Cannot inspect user settings.ini", settings, error);
    if (present) text = read_settings(settings);
    const auto entry = locate_data_path(text);
    const std::wstring newline = text.find(L"\r\n") != std::wstring::npos ? L"\r\n" : L"\n";
    if (entry.value) {
        text.replace(entry.value_begin, entry.value_end - entry.value_begin, selected.wstring());
    } else if (entry.section_end) {
        const auto offset = *entry.section_end;
        const bool needs_newline = offset != 0 && text[offset - 1] != L'\n';
        text.insert(offset, (needs_newline ? newline : L"") +
                            std::wstring(L"data_path=") + selected.wstring() + newline);
    } else {
        if (!text.empty() && text.back() != L'\n') text += newline;
        text += L"[Paths]" + newline + L"data_path=" + selected.wstring() + newline;
    }
    const std::string bytes = "\xef\xbb\xbf" + encode_utf8(text);
    if (bytes.size() > kMaxSettingsBytes) {
        throw std::runtime_error("User settings.ini exceeds the size limit");
    }

    fs::path temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 32; ++attempt) {
        temporary = settings;
        temporary += L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
                     std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Create user settings.ini temporary file");
        }
    }
    if (file == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot reserve a user settings.ini temporary file");
    }
    try {
        DWORD written = 0;
        if (!WriteFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Write user settings.ini");
        }
        if (written != bytes.size()) {
            throw std::runtime_error("User settings.ini was only partially written");
        }
        if (!FlushFileBuffers(file)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Flush user settings.ini");
        }
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Close user settings.ini");
        }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), settings.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                    "Replace user settings.ini");
        }
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw;
    }
}

[[nodiscard]] bool path_prefix(const fs::path& prefix, const fs::path& path) {
    auto lhs = prefix.begin();
    auto rhs = path.begin();
    for (; lhs != prefix.end(); ++lhs, ++rhs) {
        if (rhs == path.end() || !same_name(lhs->native(), rhs->native())) return false;
    }
    return true;
}

void ensure_separate_directories(const fs::path& data, const fs::path& user) {
    // weakly_canonical follows existing junctions/symlinks even if user_dir is
    // not yet created. Reject ancestors too: --user-dir must not put settings
    // alongside the supplied original files (including an original sauce/).
    const auto actual_data = fs::weakly_canonical(data);
    const auto actual_user = fs::weakly_canonical(user);
    const auto actual_saves = fs::weakly_canonical(user / L"saves");
    if (path_prefix(actual_data, actual_user) || path_prefix(actual_user, actual_data) ||
        path_prefix(actual_data, actual_saves) || path_prefix(actual_saves, actual_data)) {
        throw std::runtime_error("User settings and saves must be outside the purchased data tree");
    }
}

void show_stale_path(HWND owner, const fs::path& path, std::wstring_view reason) {
    const std::wstring message = L"The saved data_path is no longer usable:\n" +
                                 path.wstring() + L"\n\n" + std::wstring(reason) +
                                 L"\n\nSelect your own purchased Windows game data folder next.";
    if (!MessageBoxW(owner, message.c_str(), L"Nitemare 3D - saved game data path",
                     MB_OK | MB_ICONWARNING | MB_TASKMODAL)) {
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
                                "Explain stale saved data_path");
    }
}

[[nodiscard]] fs::path pick_data_directory(HWND owner) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    check_hresult(initialized, "Initialize COM STA for the game data picker");
    struct Apartment final {
        ~Apartment() { CoUninitialize(); }
    } apartment;

    ComPtr<IFileOpenDialog> dialog;
    check_hresult(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(dialog.GetAddressOf())), "Create game data folder picker");
    DWORD options = 0;
    check_hresult(dialog->GetOptions(&options), "Read game data folder picker options");
    check_hresult(dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM |
                                     FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR),
                  "Set game data folder picker options");
    check_hresult(dialog->SetTitle(L"Select your purchased Windows Nitemare 3D data folder"),
                  "Title game data folder picker");
    check_hresult(dialog->SetOkButtonLabel(L"Use this folder"),
                  "Label game data folder picker button");
    ComPtr<IFileDialogCustomize> customize;
    check_hresult(dialog.As(&customize), "Explain game data ownership in picker");
    check_hresult(customize->AddText(1001,
                  L"Use only data from your own purchased Windows copy. Files are read-only; "
                  L"settings and saves are stored separately. No original executable is run."),
                  "Show game data ownership explanation");
    const HRESULT shown = dialog->Show(owner);
    if (shown == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        throw std::runtime_error("Game data folder selection was cancelled");
    }
    check_hresult(shown, "Show game data folder picker");
    ComPtr<IShellItem> chosen;
    check_hresult(dialog->GetResult(chosen.GetAddressOf()), "Read selected game data folder");
    PWSTR raw = nullptr;
    check_hresult(chosen->GetDisplayName(SIGDN_FILESYSPATH, &raw),
                  "Get selected game data folder path");
    const std::unique_ptr<wchar_t, decltype(&CoTaskMemFree)> owned(raw, CoTaskMemFree);
    return absolute_once(fs::path(owned.get()));
}

} // namespace

LaunchOptions parse_launch_options(int argc, const wchar_t* const* argv) {
    if (argc < 1 || !argv || !argv[0]) {
        throw std::invalid_argument("Wide Unicode command-line arguments are required");
    }
    LaunchOptions options;
    for (int index = 1; index < argc; ++index) {
        if (!argv[index]) throw std::invalid_argument("Null command-line argument");
        const std::wstring_view argument(argv[index]);
        if (argument == L"--data" || argument == L"--user-dir") {
            if (index + 1 >= argc || !argv[index + 1] || !*argv[index + 1] ||
                std::wstring_view(argv[index + 1]).starts_with(L"--")) {
                throw std::invalid_argument(argument == L"--data"
                    ? "--data requires a directory" : "--user-dir requires a directory");
            }
            const auto absolute = absolute_once(fs::path(argv[++index]));
            auto& target = argument == L"--data" ? options.data_directory : options.user_directory;
            if (target) throw std::invalid_argument(argument == L"--data"
                ? "--data specified more than once" : "--user-dir specified more than once");
            target = absolute;
        } else if (argument == L"--maps") {
            if (options.maps) throw std::invalid_argument("--maps specified more than once");
            options.maps = true;
        } else if (argument == L"--square-pixels") {
            if (options.square_pixels) throw std::invalid_argument("--square-pixels specified more than once");
            options.square_pixels = true;
        } else if (argument == L"--modern") {
            if (options.modern_controls) throw std::invalid_argument("--modern specified more than once");
            options.modern_controls = true;
        } else {
            throw std::invalid_argument("Unknown Nitemare 3D command-line option");
        }
    }
    return options;
}

ResolvedPaths resolve_paths(const LaunchOptions& options, HWND owner) {
    const fs::path user = options.user_directory ? *options.user_directory : default_user_directory();
    const fs::path settings = user / L"settings.ini";
    const fs::path saves = user / L"saves";
    const auto finish = [&](n3d::Assets&& assets, bool remember) -> ResolvedPaths {
        const auto& selected = assets.directory;
        ensure_separate_directories(selected, user);
        fs::create_directories(saves);
        if (remember) save_data_directory(settings, selected);
        return ResolvedPaths{std::move(assets), user, saves, options.maps,
                             options.square_pixels, options.modern_controls};
    };

    if (options.data_directory) return finish(n3d::load_assets(*options.data_directory), false);

    const fs::path bundled = (executable_directory() / L"data").lexically_normal();
    std::error_code error;
    const bool present = fs::exists(bundled, error);
    if (error) throw fs::filesystem_error("Cannot inspect adjacent data directory", bundled, error);
    if (present) return finish(n3d::load_assets(bundled), false);

    if (auto saved = saved_data_directory(settings)) {
        if (saved->empty() || !saved->is_absolute()) {
            show_stale_path(owner, *saved, L"Saved data_path must be an absolute directory path.");
        } else {
            std::optional<n3d::Assets> validated;
            try {
                validated.emplace(n3d::load_assets(saved->lexically_normal()));
            } catch (const n3d::DataError& problem) {
                show_stale_path(owner, *saved,
                    decode_diagnostic(problem.path()) + L" at byte offset " +
                    std::to_wstring(problem.offset()) + L": " + decode_utf8(problem.invariant()));
            } catch (const fs::filesystem_error& problem) {
                show_stale_path(owner, *saved, decode_diagnostic(problem.what()));
            }
            if (validated) return finish(std::move(*validated), false);
        }
    }

    // An invalid picked directory is an error, not an opportunity to silently
    // choose another source. The next launch can show the picker again.
    return finish(n3d::load_assets(pick_data_directory(owner)), true);
}

} // namespace n3d::platform
