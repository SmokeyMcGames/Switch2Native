#pragma once
#include <windows.h>
#include <shlobj.h>
#include <memory>
#include <string>
#include <type_traits>

namespace switch2::tray {
inline constexpr wchar_t PreferenceKey[] = L"SOFTWARE\\Switch2Native";
inline bool always_show() {
    DWORD value = 1, size = sizeof(value);
    RegGetValueW(HKEY_CURRENT_USER, PreferenceKey, L"AlwaysShowTray", RRF_RT_REG_DWORD,
                 nullptr, &value, &size);
    return value != 0;
}
inline bool save_preference(bool show) {
    HKEY key{};
    if (RegCreateKeyExW(HKEY_CURRENT_USER, PreferenceKey, 0, nullptr, 0, KEY_SET_VALUE,
                        nullptr, &key, nullptr) != ERROR_SUCCESS) return false;
    const DWORD value = show;
    const auto result = RegSetValueExW(key, L"AlwaysShowTray", 0, REG_DWORD,
                                     reinterpret_cast<const BYTE *>(&value), sizeof(value));
    RegCloseKey(key);
    return result == ERROR_SUCCESS;
}
inline std::wstring resolve_icon_path(std::wstring path) {
    // Explorer may store a Known Folder GUID rather than C:\Program Files.
    if (path.starts_with(L"{")) {
        const auto end = path.find(L'}');
        if (end == std::wstring::npos) return {};
        GUID folder{};
        PWSTR location{};
        if (FAILED(CLSIDFromString(path.substr(0, end + 1).c_str(), &folder)) ||
            FAILED(SHGetKnownFolderPath(folder, 0, nullptr, &location))) return {};
        path = std::wstring(location) + path.substr(end + 1);
        CoTaskMemFree(location);
    }
    wchar_t expanded[32768]{};
    const auto length = ExpandEnvironmentStringsW(path.c_str(), expanded, ARRAYSIZE(expanded));
    return length && length <= ARRAYSIZE(expanded) ? std::wstring(expanded) : std::wstring{};
}
inline bool apply_visibility(bool show) {
    // Windows 11's per-user, per-executable preference. There is no documented
    // Shell_NotifyIcon flag to promote an icon. Touch only this executable's
    // entries; do not alter Explorer-wide visibility or restart the shell.
    using Key = std::unique_ptr<std::remove_pointer_t<HKEY>, decltype(&RegCloseKey)>;
    HKEY raw{};
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0,
                      KEY_ENUMERATE_SUB_KEYS, &raw) != ERROR_SUCCESS) return false;
    Key root(raw, RegCloseKey);
    wchar_t executable[32768]{};
    const auto length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length >= ARRAYSIZE(executable)) return false;
    bool updated = false;
    for (DWORD index = 0;; ++index) {
        wchar_t name[256]{};
        DWORD size = ARRAYSIZE(name);
        const auto next = RegEnumKeyExW(root.get(), index, name, &size, nullptr, nullptr, nullptr, nullptr);
        if (next == ERROR_NO_MORE_ITEMS) break;
        if (next != ERROR_SUCCESS) break;
        HKEY child_raw{};
        if (RegOpenKeyExW(root.get(), name, 0, KEY_QUERY_VALUE | KEY_SET_VALUE, &child_raw) != ERROR_SUCCESS) continue;
        Key child(child_raw, RegCloseKey);
        wchar_t path[32768]{};
        size = sizeof(path);
        if (RegGetValueW(child.get(), nullptr, L"ExecutablePath", RRF_RT_REG_SZ,
                         nullptr, path, &size) != ERROR_SUCCESS) continue;
        if (_wcsicmp(resolve_icon_path(path).c_str(), executable) != 0) continue;
        const DWORD value = show;
        updated |= RegSetValueExW(child.get(), L"IsPromoted", 0, REG_DWORD,
                                  reinterpret_cast<const BYTE *>(&value), sizeof(value)) == ERROR_SUCCESS;
    }
    return updated;
}
} // namespace switch2::tray
