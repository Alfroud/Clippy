#include "clippy.h"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <limits>
#include <map>

namespace clippy {
namespace {
constexpr UINT kHotkeyModifiers = MOD_CONTROL | MOD_ALT | MOD_SHIFT | MOD_WIN;
constexpr int kMaxLoadDelayMs = 60000;
constexpr int kMinUploadTimeoutMs = 2000;
constexpr int kMaxUploadTimeoutMs = 120000;
constexpr int kMinFloatingOpacityPercent = 60;
constexpr int kMaxFloatingOpacityPercent = 100;
constexpr DWORD kMaxSettingsBytes = 65536;

std::wstring Trim(const std::wstring& value) {
    const auto first = value.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return {};
    const auto last = value.find_last_not_of(L" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::wstring Lower(std::wstring value) {
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return value;
}

bool IsSupportedKey(UINT key) {
    return (key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9') ||
        (key >= VK_F1 && key <= VK_F24 && key != VK_F12) || key == VK_SPACE;
}

bool IsValidHotkey(const Hotkey& hotkey) {
    return !(hotkey.modifiers & ~kHotkeyModifiers) && IsSupportedKey(hotkey.key);
}

bool ParseInteger(const std::wstring& text, int& value) {
    const auto trimmed = Trim(text);
    if (trimmed.empty()) return false;
    wchar_t* end = nullptr;
    errno = 0;
    const auto parsed = std::wcstoll(trimmed.c_str(), &end, 10);
    if (errno == ERANGE || end != trimmed.c_str() + trimmed.size() ||
        parsed < std::numeric_limits<int>::min() ||
        parsed > std::numeric_limits<int>::max()) return false;
    value = static_cast<int>(parsed);
    return true;
}

bool ParseBoolean(const std::wstring& text, bool& value) {
    const auto token = Lower(Trim(text));
    if (token == L"1" || token == L"true" || token == L"yes" || token == L"on") {
        value = true;
        return true;
    }
    if (token == L"0" || token == L"false" || token == L"no" || token == L"off") {
        value = false;
        return true;
    }
    return false;
}

std::map<std::wstring, std::wstring> ReadIniValues(const std::wstring& text) {
    std::map<std::wstring, std::wstring> values;
    bool inSection = false;
    size_t begin = 0;
    while (begin < text.size()) {
        const auto newline = text.find(L'\n', begin);
        auto line = Trim(text.substr(begin, newline == std::wstring::npos ?
            std::wstring::npos : newline - begin));
        begin = newline == std::wstring::npos ? text.size() : newline + 1;
        if (!line.empty() && line.front() == 0xfeff) line = Trim(line.substr(1));
        if (line.empty() || line.front() == L';' || line.front() == L'#') continue;
        if (line.front() == L'[') {
            inSection = line.size() >= 2 && line.back() == L']' &&
                Lower(Trim(line.substr(1, line.size() - 2))) == L"clippy";
            continue;
        }
        if (!inSection) continue;
        const auto equals = line.find(L'=');
        if (equals == std::wstring::npos) continue;
        auto key = Lower(Trim(line.substr(0, equals)));
        if (!key.empty()) values[key] = Trim(line.substr(equals + 1));
    }
    return values;
}

Settings SettingsFromText(const std::wstring& text) {
    Settings settings;
    const auto values = ReadIniValues(text);
    const auto get = [&](const wchar_t* name) -> const std::wstring* {
        const auto it = values.find(Lower(name));
        return it == values.end() ? nullptr : &it->second;
    };
    std::wstring ignoredError;
    if (const auto* value = get(L"paste")) ParseHotkey(*value, settings.paste, ignoredError);
    if (const auto* value = get(L"pasteSend")) ParseHotkey(*value, settings.pasteSend, ignoredError);
    if (const auto* value = get(L"mode")) {
        int mode = 0;
        if (ParseInteger(*value, mode) && mode >= 1 && mode <= 4)
            settings.mode = static_cast<CaptureMode>(mode);
    }
    const auto readBool = [&](const wchar_t* name, bool& destination) {
        if (const auto* value = get(name)) ParseBoolean(*value, destination);
    };
    readBool(L"floating", settings.floating);
    readBool(L"minimizeChrome", settings.minimizeChrome);
    readBool(L"modeKeys", settings.modeKeys);
    readBool(L"numbersCapture", settings.numbersCapture);
    if (const auto* value = get(L"floatingOpacityPercent")) {
        int opacity = 0;
        if (ParseInteger(*value, opacity)) settings.floatingOpacityPercent =
            std::clamp(opacity, kMinFloatingOpacityPercent, kMaxFloatingOpacityPercent);
    }
    if (const auto* value = get(L"loadDelayMs")) {
        int delay = 0;
        if (ParseInteger(*value, delay)) settings.loadDelayMs = std::clamp(delay, 0, kMaxLoadDelayMs);
    }
    if (const auto* value = get(L"uploadTimeoutMs")) {
        int timeout = 0;
        if (ParseInteger(*value, timeout)) settings.uploadTimeoutMs =
            std::clamp(timeout, kMinUploadTimeoutMs, kMaxUploadTimeoutMs);
    }
    if (const auto* value = get(L"floatingX")) ParseInteger(*value, settings.floatingX);
    if (const auto* value = get(L"floatingY")) ParseInteger(*value, settings.floatingY);
    return settings;
}

std::wstring SerializeSettings(const Settings& settings) {
    std::wstring text = L"[Clippy]\r\n";
    const auto add = [&](const wchar_t* name, const std::wstring& value) {
        text += name;
        text += L"=" + value + L"\r\n";
    };
    const auto addBool = [&](const wchar_t* name, bool value) { add(name, value ? L"1" : L"0"); };
    add(L"paste", HotkeyLabel(settings.paste));
    add(L"pasteSend", HotkeyLabel(settings.pasteSend));
    add(L"mode", std::to_wstring(static_cast<int>(settings.mode)));
    addBool(L"floating", settings.floating);
    add(L"floatingOpacityPercent", std::to_wstring(std::clamp(settings.floatingOpacityPercent,
        kMinFloatingOpacityPercent, kMaxFloatingOpacityPercent)));
    addBool(L"minimizeChrome", settings.minimizeChrome);
    addBool(L"modeKeys", settings.modeKeys);
    addBool(L"numbersCapture", settings.numbersCapture);
    add(L"loadDelayMs", std::to_wstring(std::clamp(settings.loadDelayMs, 0, kMaxLoadDelayMs)));
    add(L"uploadTimeoutMs", std::to_wstring(std::clamp(settings.uploadTimeoutMs,
        kMinUploadTimeoutMs, kMaxUploadTimeoutMs)));
    add(L"floatingX", std::to_wstring(settings.floatingX));
    add(L"floatingY", std::to_wstring(settings.floatingY));
    return text;
}

std::wstring WindowsError(const wchar_t* action, DWORD code) {
    wchar_t* message = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, reinterpret_cast<wchar_t*>(&message), 0, nullptr);
    std::wstring result(action);
    result += L" (Windows error " + std::to_wstring(code) + L")";
    if (length && message) result += L": " + Trim(std::wstring(message, length));
    if (message) LocalFree(message);
    return result;
}

std::wstring EnvironmentValue(const wchar_t* name) {
    DWORD length = GetEnvironmentVariableW(name, nullptr, 0);
    if (!length) return {};
    std::wstring value(length, L'\0');
    const DWORD copied = GetEnvironmentVariableW(name, value.data(), length);
    if (!copied || copied >= length) return {};
    value.resize(copied);
    return value;
}

bool ReadSettingsFile(const std::wstring& path, std::wstring& text) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE |
        FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart < 0 || size.QuadPart > kMaxSettingsBytes) {
        CloseHandle(file);
        return false;
    }
    std::vector<char> bytes(static_cast<size_t>(size.QuadPart));
    DWORD read = 0;
    const bool readOk = bytes.empty() ||
        (ReadFile(file, bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr) && read == bytes.size());
    CloseHandle(file);
    if (!readOk) return false;
    if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff &&
        static_cast<unsigned char>(bytes[1]) == 0xfe) {
        if (bytes.size() % 2) return false;
        text.clear();
        text.reserve((bytes.size() - 2) / 2);
        for (size_t i = 2; i < bytes.size(); i += 2) {
            const auto character = static_cast<wchar_t>(static_cast<unsigned char>(bytes[i]) |
                (static_cast<unsigned int>(static_cast<unsigned char>(bytes[i + 1])) << 8));
            if (!character) return false;
            text.push_back(character);
        }
        return true;
    }
    const size_t begin = bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xef &&
        static_cast<unsigned char>(bytes[1]) == 0xbb && static_cast<unsigned char>(bytes[2]) == 0xbf ? 3 : 0;
    if (begin == bytes.size()) { text.clear(); return true; }
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + begin,
        static_cast<int>(bytes.size() - begin), nullptr, 0);
    if (!count) return false;
    text.resize(count);
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data() + begin,
        static_cast<int>(bytes.size() - begin), text.data(), count)) return false;
    return text.find(L'\0') == std::wstring::npos;
}
} // namespace

std::wstring SettingsPath() {
    auto directory = EnvironmentValue(L"LOCALAPPDATA");
    if (directory.empty()) {
        directory = EnvironmentValue(L"USERPROFILE");
        if (directory.empty()) return {};
        directory += L"\\AppData\\Local";
    }
    while (!directory.empty() && (directory.back() == L'\\' || directory.back() == L'/'))
        directory.pop_back();
    return directory + L"\\Clippy\\settings.ini";
}

std::wstring HotkeyLabel(const Hotkey& hotkey) {
    std::wstring label;
    if (hotkey.modifiers & MOD_CONTROL) label += L"Ctrl+";
    if (hotkey.modifiers & MOD_ALT) label += L"Alt+";
    if (hotkey.modifiers & MOD_SHIFT) label += L"Shift+";
    if (hotkey.modifiers & MOD_WIN) label += L"Win+";
    if ((hotkey.key >= 'A' && hotkey.key <= 'Z') || (hotkey.key >= '0' && hotkey.key <= '9'))
        label.push_back(static_cast<wchar_t>(hotkey.key));
    else if (hotkey.key >= VK_F1 && hotkey.key <= VK_F24)
        label += L"F" + std::to_wstring(hotkey.key - VK_F1 + 1);
    else if (hotkey.key == VK_SPACE) label += L"Space";
    else label += L"Unknown";
    return label;
}

bool ParseHotkey(const std::wstring& text, Hotkey& hotkey, std::wstring& error) {
    error.clear();
    Hotkey parsed{0, 0};
    const auto input = Trim(text);
    if (input.empty()) { error = L"Enter a shortcut, such as Ctrl+Alt+K."; return false; }
    size_t begin = 0;
    do {
        const auto plus = input.find(L'+', begin);
        const auto token = Lower(Trim(input.substr(begin, plus == std::wstring::npos ?
            std::wstring::npos : plus - begin)));
        if (token.empty()) { error = L"The shortcut contains an empty key name."; return false; }
        UINT modifier = 0;
        if (token == L"ctrl" || token == L"control") modifier = MOD_CONTROL;
        else if (token == L"alt") modifier = MOD_ALT;
        else if (token == L"shift") modifier = MOD_SHIFT;
        else if (token == L"win" || token == L"windows") modifier = MOD_WIN;
        if (modifier) {
            if (parsed.modifiers & modifier) { error = L"Each shortcut modifier can appear only once."; return false; }
            parsed.modifiers |= modifier;
        } else {
            UINT key = 0;
            if (token.size() == 1 && ((token[0] >= L'a' && token[0] <= L'z') ||
                (token[0] >= L'0' && token[0] <= L'9')))
                key = static_cast<UINT>(std::towupper(token[0]));
            else if (token == L"space") key = VK_SPACE;
            else if (token.size() >= 2 && token[0] == L'f') {
                int number = 0;
                const auto suffix = token.substr(1);
                if (std::all_of(suffix.begin(), suffix.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }) &&
                    ParseInteger(suffix, number) && number >= 1 && number <= 24 &&
                    suffix == std::to_wstring(number)) key = VK_F1 + number - 1;
            }
            if (!key) { error = L"Use a letter, digit, Space, or F1 through F24."; return false; }
            if (key == VK_F12) { error = L"F12 is reserved by Windows for debugging. Choose another key."; return false; }
            if (parsed.key) { error = L"Use one key with optional Ctrl, Alt, Shift, or Win modifiers."; return false; }
            parsed.key = key;
        }
        if (plus == std::wstring::npos) break;
        begin = plus + 1;
    } while (begin <= input.size());
    if (!parsed.key) { error = L"Add a key after the shortcut modifiers."; return false; }
    hotkey = parsed;
    return true;
}

Settings LoadSettings() {
    std::wstring text;
    const auto path = SettingsPath();
    return !path.empty() && ReadSettingsFile(path, text) ? SettingsFromText(text) : Settings{};
}

bool SaveSettings(const Settings& settings, std::wstring& error) {
    error.clear();
    if (!IsValidHotkey(settings.paste) || !IsValidHotkey(settings.pasteSend)) {
        error = L"The settings contain an unsupported shortcut.";
        return false;
    }
    const int mode = static_cast<int>(settings.mode);
    if (mode < 1 || mode > 4) { error = L"Choose a valid capture mode."; return false; }
    const auto path = SettingsPath();
    if (path.empty()) { error = L"Windows did not provide a local application data folder."; return false; }
    const auto directory = path.substr(0, path.find_last_of(L'\\'));
    if (!CreateDirectoryW(directory.c_str(), nullptr)) {
        const DWORD code = GetLastError();
        if (code != ERROR_ALREADY_EXISTS) {
            error = WindowsError(L"Could not create the Clippy settings folder", code);
            return false;
        }
        const DWORD attributes = GetFileAttributesW(directory.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || !(attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            error = L"The Clippy settings folder path is occupied by a file.";
            return false;
        }
    }
    // Write our own UTF-16 INI text instead of using profile APIs, whose cache
    // would otherwise need flushing around an atomic file replacement.
    std::wstring temporary;
    HANDLE file = INVALID_HANDLE_VALUE;
    DWORD createError = ERROR_FILE_EXISTS;
    for (int attempt = 0; attempt < 32; ++attempt) {
        temporary = path + L".tmp." + std::to_wstring(GetCurrentProcessId()) + L"." +
            std::to_wstring(GetTickCount64()) + L"." + std::to_wstring(attempt);
        file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file != INVALID_HANDLE_VALUE) break;
        createError = GetLastError();
        if (createError != ERROR_FILE_EXISTS && createError != ERROR_ALREADY_EXISTS) break;
    }
    if (file == INVALID_HANDLE_VALUE) {
        error = WindowsError(L"Could not create a temporary settings file", createError);
        return false;
    }
    const std::wstring text = L"\xfeff" + SerializeSettings(settings);
    const DWORD byteCount = static_cast<DWORD>(text.size() * sizeof(wchar_t));
    DWORD written = 0;
    DWORD writeError = ERROR_SUCCESS;
    if (!WriteFile(file, text.data(), byteCount, &written, nullptr)) writeError = GetLastError();
    else if (written != byteCount) writeError = ERROR_WRITE_FAULT;
    else if (!FlushFileBuffers(file)) writeError = GetLastError();
    if (!CloseHandle(file) && writeError == ERROR_SUCCESS) writeError = GetLastError();
    if (writeError != ERROR_SUCCESS) {
        DeleteFileW(temporary.c_str());
        error = WindowsError(L"Could not write the Clippy settings", writeError);
        return false;
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD code = GetLastError();
        DeleteFileW(temporary.c_str());
        error = WindowsError(L"Could not replace the Clippy settings file", code);
        return false;
    }
    return true;
}

int RunSelfTests() {
    int failures = 0;
    int checks = 0;
    const auto check = [&](bool passed, const char* label) {
        ++checks;
        if (!passed) { ++failures; std::printf("FAIL: %s\n", label); }
    };
    const auto sameKey = [](const Hotkey& a, const Hotkey& b) {
        return a.modifiers == b.modifiers && a.key == b.key;
    };
    std::wstring error;
    Hotkey key;
    check(ParseHotkey(L" control + ALT + k ", key, error) &&
        sameKey(key, Hotkey{MOD_CONTROL | MOD_ALT, 'K'}), "trimmed modifier aliases and lowercase key");
    check(ParseHotkey(L"Shift+Win+F24", key, error) &&
        sameKey(key, Hotkey{MOD_SHIFT | MOD_WIN, VK_F24}), "F24 upper bound");
    check(ParseHotkey(L"Alt+Space", key, error) && key.key == VK_SPACE, "named Space key");
    check(ParseHotkey(L"1", key, error) && key.modifiers == 0 && key.key == '1',
        "unmodified digits parse for contextual conflict validation");
    const wchar_t* invalid[] = {L"", L" ", L"Ctrl", L"Ctrl+Alt", L"Ctrl++K", L"+K",
        L"Ctrl+K+", L"Ctrl+Ctrl+K", L"Ctrl+Control+K", L"Alt+K+L", L"F12", L"Ctrl+F12",
        L"F0", L"F25", L"F01", L"F+1", L"F-1", L"F2147483648", L"Enter", L"Ctrl+!"};
    for (const auto* input : invalid) {
        const Hotkey original{MOD_SHIFT, 'Z'};
        key = original;
        check(!ParseHotkey(input, key, error) && !error.empty() && sameKey(key, original),
            "invalid shortcuts fail without modifying the destination");
    }
    for (UINT modifiers = 0; modifiers <= kHotkeyModifiers; ++modifiers) {
        const UINT keys[] = {'A', 'Z', '0', '9', VK_F1, VK_F11, VK_F13, VK_F24, VK_SPACE};
        for (const UINT code : keys) {
            const Hotkey original{modifiers, code};
            check(ParseHotkey(HotkeyLabel(original), key, error) && sameKey(key, original),
                "shortcut label and parser round trip");
        }
    }
    const Settings defaults = SettingsFromText(L"");
    check(sameKey(defaults.paste, Hotkey{0, 'K'}) &&
        sameKey(defaults.pasteSend, Hotkey{MOD_SHIFT, 'K'}) &&
        defaults.mode == CaptureMode::AllMonitors && defaults.floating && !defaults.minimizeChrome &&
        defaults.modeKeys && !defaults.numbersCapture && defaults.loadDelayMs == 3000 &&
        defaults.uploadTimeoutMs == 20000 && defaults.floatingX == -1 && defaults.floatingY == -1 &&
        defaults.floatingOpacityPercent == 88,
        "empty config preserves all defaults");
    const Settings damaged = SettingsFromText(L"[Clippy]\nPaste=Ctrl+F12\nPasteSend=Ctrl+Ctrl+K\n"
        L"mode=9\nfloating=maybe\nloadDelayMs=garbage\nuploadTimeoutMs=2147483648\n"
        L"floatingX=abc\nfloatingY=1xyz\nfloatingOpacityPercent=invalid\n");
    check(SerializeSettings(damaged) == SerializeSettings(defaults), "invalid config values preserve defaults");
    Settings custom;
    custom.paste = {MOD_WIN | MOD_SHIFT, VK_F13};
    custom.pasteSend = {MOD_CONTROL, '9'};
    custom.mode = CaptureMode::Region;
    custom.floating = false;
    custom.floatingOpacityPercent = 67;
    custom.minimizeChrome = true;
    custom.modeKeys = false;
    custom.numbersCapture = true;
    custom.loadDelayMs = 60000;
    custom.uploadTimeoutMs = 2000;
    custom.floatingX = -1920;
    custom.floatingY = 700;
    check(SerializeSettings(SettingsFromText(SerializeSettings(custom))) == SerializeSettings(custom),
        "all config fields round trip in memory without writing settings");
    const auto clamped = SettingsFromText(L"[Other]\nmode=4\n[ CLIPPY ]\nloadDelayMs=-5\n"
        L"uploadTimeoutMs=999999\nfloating=OFF\nMODE=3\nfloatingX=-3840\n");
    check(clamped.loadDelayMs == 0 && clamped.uploadTimeoutMs == 120000 && !clamped.floating &&
        clamped.mode == CaptureMode::AllMonitors && clamped.floatingX == -3840,
        "config section matching, booleans, timing clamp, and negative coordinates");
    check(SettingsFromText(L"[Clippy]\nfloatingOpacityPercent=59\n").floatingOpacityPercent == 60,
        "opacity below readable range clamps to sixty percent");
    check(SettingsFromText(L"[Clippy]\nfloatingOpacityPercent=101\n").floatingOpacityPercent == 100,
        "opacity above full opacity clamps to one hundred percent");
    for (int opacity : {60, 100}) {
        Settings boundary;
        boundary.floatingOpacityPercent = opacity;
        check(SettingsFromText(SerializeSettings(boundary)).floatingOpacityPercent == opacity,
            "supported opacity boundaries persist across config serialization");
    }
    const RECT empty = UnionMonitors({});
    check(empty.left == 0 && empty.top == 0 && empty.right == 0 && empty.bottom == 0,
        "empty monitor union");
    const std::vector<Monitor> monitors = {
        {{0, 0, 1920, 1080}, L"Primary", true},
        {{-2560, -200, 0, 1240}, L"Left", false},
        {{500, -1440, 3060, 0}, L"Above", false}
    };
    const RECT bounds = UnionMonitors(monitors);
    check(bounds.left == -2560 && bounds.top == -1440 && bounds.right == 3060 && bounds.bottom == 1240,
        "monitor union includes negative and offset monitor coordinates");
    const RECT single = UnionMonitors({monitors[1]});
    check(single.left == -2560 && single.top == -200 && single.right == 0 && single.bottom == 1240,
        "single negative-coordinate monitor union");
    std::printf("Clippy self-test: %d checks, %d failures.\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
} // namespace clippy
