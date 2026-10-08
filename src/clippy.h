#pragma once
#ifndef UNICODE
#define UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>
#include <vector>
#include <functional>
#include <cstdint>

namespace clippy {
enum class CaptureMode { Monitor1 = 1, Monitor2 = 2, AllMonitors = 3, Region = 4 };
struct Hotkey { UINT modifiers = 0; UINT key = 'K'; };
struct Settings {
    Hotkey paste;
    Hotkey pasteSend{MOD_SHIFT, 'K'};
    CaptureMode mode = CaptureMode::AllMonitors;
    bool floating = true;
    int floatingOpacityPercent = 88;
    bool minimizeChrome = false;
    bool modeKeys = true;
    bool numbersCapture = false;
    int loadDelayMs = 3000;
    int uploadTimeoutMs = 20000;
    int floatingX = -1;
    int floatingY = -1;
};
struct Monitor { RECT bounds{}; std::wstring name; bool primary = false; };
struct Screenshot { int width = 0; int height = 0; std::vector<std::uint8_t> dib; };
struct DeliveryResult { bool success = false; bool sent = false; std::wstring message; };

std::vector<Monitor> GetMonitors();
RECT UnionMonitors(const std::vector<Monitor>& monitors);
bool CaptureRect(const RECT& rect, Screenshot& shot, std::wstring& error);
bool SetScreenshotClipboard(HWND owner, const Screenshot& shot, std::wstring& error);
bool SelectRegion(HINSTANCE instance, HWND owner, RECT& selected);

Settings LoadSettings();
bool SaveSettings(const Settings& settings, std::wstring& error);
std::wstring SettingsPath();
std::wstring HotkeyLabel(const Hotkey& key);
bool ParseHotkey(const std::wstring& text, Hotkey& hotkey, std::wstring& error);
int RunSelfTests();

DeliveryResult DeliverToChatGPT(const Settings& settings, bool send,
    HWND previousWindow, const std::function<void(const std::wstring&)>& report,
    const std::function<bool()>& cancelled = {});
bool ProbeChrome(std::wstring& report);
}
