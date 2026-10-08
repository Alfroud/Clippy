#include "clippy.h"
#include <shellapi.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <memory>
#include <thread>
#include <cstdio>
#include <cwchar>
#include <atomic>

using namespace clippy;
namespace {
constexpr UINT WM_STATUS = WM_APP + 1, WM_DONE = WM_APP + 2, WM_TRAY = WM_APP + 3;
constexpr int HK_PASTE = 1, HK_SEND = 2, HK_TOGGLE = 3, HK_MODE = 10;
constexpr int CMD_SETTINGS = 100, CMD_TOGGLE = 101, CMD_FLOAT = 102, CMD_EXIT = 103;
constexpr int CMD_MINIMIZE = 104;
constexpr int ID_PASTE = 201, ID_SEND = 202, ID_FLOAT = 203, ID_NUMBERS = 204;
constexpr int ID_RETURN = 205, ID_DELAY = 206, ID_TIMEOUT = 207, ID_SAVE = 208;
constexpr int ID_STATUS = 209, ID_ARM = 210, ID_OPACITY = 211, ID_OPACITY_VALUE = 212;
constexpr COLORREF BG = RGB(19, 27, 39), PANEL = RGB(32, 44, 59);
constexpr COLORREF INK = RGB(240, 246, 252), MUTED = RGB(161, 177, 193), TEAL = RGB(72, 221, 186);
HINSTANCE instance;
HWND controller = nullptr, floating = nullptr, settingsWindow = nullptr, floatingTooltips = nullptr;
HFONT uiFont = nullptr, titleFont = nullptr, keyFont = nullptr;
HFONT settingsFont = nullptr;
HBRUSH settingsBackgroundBrush = nullptr, settingsPanelBrush = nullptr;
Settings settings;
bool armed = false, busy = false, exiting = false, suppressToggle = false;
std::atomic_bool cancelRequested = false;
bool restoreSettings = false;
std::wstring status = L"Paused — Ctrl+Alt+Space to start";
HWND lastExternalWindow = nullptr;
HWINEVENTHOOK foregroundHook = nullptr;
std::thread deliveryThread;
RECT lastRegion{};
bool hasRegion = false;
int pulse = 0;
int hoveredHeaderButton = 0;
bool trackingFloatingMouse = false;
HICON appIcon = nullptr;
constexpr RECT FLOAT_BADGE{98, 10, 200, 32};
constexpr RECT FLOAT_MINIMIZE{207, 7, 234, 35};
constexpr RECT FLOAT_CLOSE{240, 7, 267, 35};

bool OurWindow(HWND window) {
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}
void CALLBACK ForegroundEvent(HWINEVENTHOOK, DWORD, HWND window, LONG, LONG, DWORD, DWORD) {
    if (window && !OurWindow(window)) lastExternalWindow = window;
}
void Refresh() { if (floating) InvalidateRect(floating, nullptr, FALSE); }
void SetStatus(const std::wstring& value) { status = value; Refresh(); }
std::wstring ModeLabel() {
    switch (settings.mode) {
    case CaptureMode::Monitor1: return L"Monitor 1";
    case CaptureMode::Monitor2: return L"Monitor 2";
    case CaptureMode::Region: return hasRegion ? L"Saved region · 4 to redraw" : L"Drag a region on next capture";
    default: return L"Both monitors";
    }
}
void Notify(const std::wstring& title, const std::wstring& message) {
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data);
    data.hWnd = controller; data.uID = 1; data.uFlags = NIF_INFO;
    wcsncpy(data.szInfoTitle, title.c_str(), 63);
    wcsncpy(data.szInfo, message.c_str(), 255);
    data.dwInfoFlags = NIIF_INFO;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}
void UnregisterCaptureKeys() {
    UnregisterHotKey(controller, HK_PASTE); UnregisterHotKey(controller, HK_SEND);
    for (int i = 1; i <= 4; ++i) UnregisterHotKey(controller, HK_MODE + i);
}
bool EqualKey(const Hotkey& left, const Hotkey& right) {
    return left.key == right.key && left.modifiers == right.modifiers;
}
bool ValidateHotkeys(const Settings& candidate, std::wstring& error) {
    if (EqualKey(candidate.paste, candidate.pasteSend)) { error = L"The two actions need different shortcuts."; return false; }
    for (const auto& key : {candidate.paste, candidate.pasteSend}) {
        if (EqualKey(key, Hotkey{MOD_CONTROL | MOD_ALT, VK_SPACE})) {
            error = L"Ctrl+Alt+Space is reserved for pausing capture."; return false;
        }
        if (candidate.modeKeys && key.modifiers == 0 && key.key >= '1' && key.key <= '4') {
            error = L"Keys 1–4 are reserved for capture modes. Disable number shortcuts to use them for an action."; return false;
        }
        if (EqualKey(key, Hotkey{MOD_CONTROL, 'V'}) || EqualKey(key, Hotkey{MOD_CONTROL | MOD_SHIFT, 'A'})) {
            error = L"Ctrl+V and Ctrl+Shift+A are used to paste and find Chrome tabs. Choose a different shortcut."; return false;
        }
    }
    return true;
}
bool RegisterCaptureKeys(std::wstring& error) {
    if (busy) { if (!armed) UnregisterCaptureKeys(); return true; }
    UnregisterCaptureKeys();
    if (!armed) return true;
    if (!ValidateHotkeys(settings, error)) return false;
    auto reg = [](int id, Hotkey key) { return RegisterHotKey(controller, id, key.modifiers | MOD_NOREPEAT, key.key) != FALSE; };
    if (!reg(HK_PASTE, settings.paste) || !reg(HK_SEND, settings.pasteSend)) {
        UnregisterCaptureKeys(); error = L"An action shortcut is already in use by another app. Change it in Settings."; return false;
    }
    if (settings.modeKeys) {
        for (int i = 1; i <= 4; ++i) if (!reg(HK_MODE + i, Hotkey{0, static_cast<UINT>('0' + i)})) {
            UnregisterCaptureKeys(); error = L"A number shortcut is already in use. Disable number shortcuts in Settings."; return false;
        }
    }
    return true;
}
void SetArmed(bool enabled) {
    armed = enabled;
    std::wstring error;
    if (!RegisterCaptureKeys(error)) { armed = false; SetStatus(error); Notify(L"Shortcut unavailable", error); }
    else if (!busy) SetStatus(armed ? L"Ready · " + ModeLabel() : L"Paused — Ctrl+Alt+Space to start");
    Refresh();
}
void SelectMode(int mode) {
    if (busy) return;
    settings.mode = static_cast<CaptureMode>(mode);
    if (mode == 4) hasRegion = false;
    SetStatus((armed ? L"Ready · " : L"Selected · ") + ModeLabel());
}
void PostStatus(const std::wstring& text) {
    auto* value = new std::wstring(text);
    if (!PostMessageW(controller, WM_STATUS, 0, reinterpret_cast<LPARAM>(value))) delete value;
}
void StartCapture(bool send) {
    if (busy || exiting) return;
    HWND previous = GetForegroundWindow();
    if (OurWindow(previous)) previous = lastExternalWindow;
    if (!previous || !IsWindow(previous)) { SetStatus(L"Open the window you want to capture first."); return; }
    busy = true;
    if (floatingTooltips) SendMessageW(floatingTooltips, TTM_POP, 0, 0);
    restoreSettings = settingsWindow && IsWindowVisible(settingsWindow);
    if (restoreSettings) ShowWindow(settingsWindow, SW_HIDE);
    auto cancel = [&](const std::wstring& message) {
        busy = false; std::wstring shortcutError;
        if (restoreSettings && settingsWindow && !exiting) ShowWindow(settingsWindow, SW_SHOWNOACTIVATE);
        restoreSettings = false;
        if (!RegisterCaptureKeys(shortcutError)) { armed = false; SetStatus(shortcutError); }
        else SetStatus(message);
    };
    const auto monitors = GetMonitors();
    RECT bounds{};
    if (settings.mode == CaptureMode::Region) {
        ShowWindow(floating, SW_HIDE);
        if (!hasRegion) {
            bool selected = SelectRegion(instance, controller, lastRegion);
            if (settings.floating) ShowWindow(floating, SW_SHOWNOACTIVATE);
            if (!selected || exiting) { cancel(L"Selection cancelled"); return; }
            hasRegion = true;
        }
        bounds = lastRegion;
    } else if (settings.mode == CaptureMode::AllMonitors) {
        auto both = monitors;
        if (both.size() > 2) both.resize(2);
        bounds = UnionMonitors(both);
    }
    else {
        size_t index = settings.mode == CaptureMode::Monitor1 ? 0 : 1;
        if (index >= monitors.size()) { cancel(L"Monitor 2 is not connected. Choose 1 or 3."); MessageBeep(MB_ICONINFORMATION); return; }
        bounds = monitors[index].bounds;
    }
    // Hide the controls before capture, even on systems without capture exclusion.
    if (floating) ShowWindow(floating, SW_HIDE);
    DwmFlush();
    Screenshot shot;
    std::wstring error;
    bool captured = CaptureRect(bounds, shot, error);
    if (settings.floating) ShowWindow(floating, SW_SHOWNOACTIVATE);
    if (!captured || !SetScreenshotClipboard(controller, shot, error)) { cancel(error); return; }
    DWORD screenshotClipboard = GetClipboardSequenceNumber();
    pulse = send ? 2 : 1; SetTimer(floating, 1, 380, nullptr);
    SetStatus(send ? L"Adding screenshot, then sending…" : L"Adding screenshot…");
    if (deliveryThread.joinable()) deliveryThread.join();
    Settings snapshot = settings;
    cancelRequested.store(false);
    deliveryThread = std::thread([snapshot, previous, send, screenshotClipboard] {
        // Wait for the physical shortcut to be released before injecting input.
        DWORD began = GetTickCount();
        auto held = [&] {
            for (int key : {VK_SHIFT, VK_CONTROL, VK_MENU, VK_LWIN, VK_RWIN, static_cast<int>(send ? snapshot.pasteSend.key : snapshot.paste.key)})
                if (GetAsyncKeyState(key) & 0x8000) return true;
            return false;
        };
        while (held() && !cancelRequested.load() && GetTickCount() - began < 4000) Sleep(20);
        DeliveryResult result;
        try {
            result = cancelRequested.load() ? DeliveryResult{false, false, L"Capture cancelled."}
                : held() ? DeliveryResult{false, false, L"Release the shortcut and try again; screenshot is on the clipboard."}
                : GetClipboardSequenceNumber() != screenshotClipboard ? DeliveryResult{false, false, L"The clipboard changed before delivery. Capture the screenshot again."}
                : DeliverToChatGPT(snapshot, send, previous, PostStatus, [] { return cancelRequested.load(); });
        } catch (...) {
            result = {false, false, L"Chrome automation failed. Screenshot remains on the clipboard."};
        }
        auto* message = new DeliveryResult(std::move(result));
        if (!PostMessageW(controller, WM_DONE, 0, reinterpret_cast<LPARAM>(message))) delete message;
    });
}
void Text(HDC dc, const std::wstring& text, RECT rect, HFONT font, COLORREF color,
    UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    auto old = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
    DrawTextW(dc, text.c_str(), -1, &rect, flags); SelectObject(dc, old);
}
void Box(HDC dc, RECT rect, COLORREF color, int radius = 12) {
    HBRUSH brush = CreateSolidBrush(color); auto oldBrush = SelectObject(dc, brush);
    auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
    RoundRect(dc, rect.left, rect.top, rect.right, rect.bottom, radius, radius);
    SelectObject(dc, oldPen); SelectObject(dc, oldBrush); DeleteObject(brush);
}
int FloatScale(HWND window, int value) { return MulDiv(value, GetDpiForWindow(window), 96); }
RECT ScaledRect(HWND window, RECT rect) {
    return {FloatScale(window, rect.left), FloatScale(window, rect.top), FloatScale(window, rect.right), FloatScale(window, rect.bottom)};
}
int HeaderButtonAt(POINT point) {
    if (PtInRect(&FLOAT_MINIMIZE, point)) return 2;
    if (PtInRect(&FLOAT_CLOSE, point)) return 3;
    if (PtInRect(&FLOAT_BADGE, point)) return 1;
    return 0;
}
void ApplyFloatingOpacity(int opacity = -1) {
    if (!floating) return;
    if (opacity < 0) opacity = settings.floatingOpacityPercent;
    const BYTE alpha = static_cast<BYTE>(MulDiv(std::clamp(opacity, 60, 100), 255, 100));
    SetLayeredWindowAttributes(floating, 0, alpha, LWA_ALPHA);
}
void UpdateFloatingTooltips() {
    if (!floatingTooltips || !floating) return;
    const RECT areas[] = {FLOAT_BADGE, FLOAT_MINIMIZE, FLOAT_CLOSE};
    for (UINT_PTR i = 0; i < 3; ++i) {
        TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.hwnd = floating;
        tool.uId = i + 1; tool.rect = ScaledRect(floating, areas[i]);
        SendMessageW(floatingTooltips, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&tool));
    }
}
void CreateFloatingTooltips() {
    floatingTooltips = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, CW_USEDEFAULT, CW_USEDEFAULT,
        CW_USEDEFAULT, CW_USEDEFAULT, floating, nullptr, instance, nullptr);
    if (!floatingTooltips) return;
    const RECT areas[] = {FLOAT_BADGE, FLOAT_MINIMIZE, FLOAT_CLOSE};
    const wchar_t* descriptions[] = {L"Arm / pause capture shortcuts (Ctrl+Alt+Space)",
        L"Minimize to tray — capture shortcuts stay available", L"Exit Clippy"};
    for (UINT_PTR i = 0; i < 3; ++i) {
        TOOLINFOW tool{}; tool.cbSize = sizeof(tool); tool.uFlags = TTF_SUBCLASS;
        tool.hwnd = floating; tool.hinst = instance; tool.uId = i + 1;
        tool.rect = ScaledRect(floating, areas[i]); tool.lpszText = const_cast<wchar_t*>(descriptions[i]);
        SendMessageW(floatingTooltips, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
    }
    SendMessageW(floatingTooltips, TTM_SETMAXTIPWIDTH, 0, FloatScale(floating, 300));
}
void PaintFloat(HWND window) {
    PAINTSTRUCT ps{}; HDC dc = BeginPaint(window, &ps);
    RECT client{}; GetClientRect(window, &client);
    HDC buffer = CreateCompatibleDC(dc); HBITMAP bitmap = CreateCompatibleBitmap(dc, client.right, client.bottom);
    auto old = SelectObject(buffer, bitmap);
    auto fontFor = [&](int pixels, int weight) {
        return CreateFontW(-FloatScale(window, pixels), 0, 0, 0, weight, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    };
    HFONT uiFont = fontFor(13, FW_NORMAL), titleFont = fontFor(19, FW_SEMIBOLD), keyFont = fontFor(21, FW_SEMIBOLD);
    HBRUSH background = CreateSolidBrush(BG); FillRect(buffer, &client, background); DeleteObject(background);
    Box(buffer, client, BG, FloatScale(window, 18));
    Text(buffer, L"clippy", ScaledRect(window, {16, 6, 88, 34}), titleFont, INK);
    Box(buffer, ScaledRect(window, FLOAT_BADGE), armed ? RGB(24, 75, 64) : PANEL, FloatScale(window, 10));
    Text(buffer, busy ? L"Working" : armed ? L"●  Armed" : L"○  Paused", ScaledRect(window, {104, 10, 194, 32}), uiFont, armed ? TEAL : MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Box(buffer, ScaledRect(window, FLOAT_MINIMIZE), hoveredHeaderButton == 2 ? RGB(48, 64, 82) : PANEL, FloatScale(window, 10));
    Box(buffer, ScaledRect(window, FLOAT_CLOSE), hoveredHeaderButton == 3 ? RGB(94, 43, 51) : PANEL, FloatScale(window, 10));
    Text(buffer, L"−", ScaledRect(window, FLOAT_MINIMIZE), titleFont, INK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(buffer, L"×", ScaledRect(window, FLOAT_CLOSE), titleFont, INK, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    RECT left = ScaledRect(window, {12, 42, 136, 110}), right = ScaledRect(window, {144, 42, 268, 110});
    Box(buffer, left, pulse == 1 ? TEAL : PANEL); Box(buffer, right, pulse == 2 ? TEAL : PANEL);
    Text(buffer, HotkeyLabel(settings.paste), ScaledRect(window, {16, 43, 132, 78}), keyFont, pulse == 1 ? BG : INK, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(buffer, HotkeyLabel(settings.pasteSend), ScaledRect(window, {148, 43, 264, 78}), keyFont, pulse == 2 ? BG : TEAL, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    Text(buffer, L"Add screenshot", ScaledRect(window, {16, 78, 132, 101}), uiFont, pulse == 1 ? BG : MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    Text(buffer, L"Add + send", ScaledRect(window, {148, 78, 264, 101}), uiFont, pulse == 2 ? BG : MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    const wchar_t* labels[] = {L"1  Screen", L"2  Screen", L"3  Both", L"4  Region"};
    for (int i = 0; i < 4; ++i) {
        RECT tile = ScaledRect(window, {12 + i * 65, 119, 72 + i * 65, 146});
        bool selected = static_cast<int>(settings.mode) == i + 1;
        Box(buffer, tile, selected ? RGB(24, 75, 64) : PANEL, 8);
        Text(buffer, labels[i], tile, uiFont, selected ? TEAL : MUTED, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    Text(buffer, status, ScaledRect(window, {14, 152, 266, 179}), uiFont, MUTED);
    Text(buffer, L"Drag title to move", ScaledRect(window, {14, 180, 180, 200}), uiFont, MUTED);
    Text(buffer, L"Settings", ScaledRect(window, {198, 180, 265, 200}), uiFont, TEAL, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
    SelectObject(buffer, old); DeleteObject(bitmap); DeleteDC(buffer); EndPaint(window, &ps);
    DeleteObject(uiFont); DeleteObject(titleFont); DeleteObject(keyFont);
}
void DrawSettingsFocus(HWND control, HDC dc, RECT bounds) {
    if (GetFocus() != control || (SendMessageW(control, WM_QUERYUISTATE, 0, 0) & UISF_HIDEFOCUS)) return;
    const int margin = FloatScale(control, 3);
    InflateRect(&bounds, -margin, -margin);
    SetTextColor(dc, TEAL); SetBkColor(dc, BG);
    DrawFocusRect(dc, &bounds);
}
void PaintDarkButton(HWND control, HDC dc) {
    RECT bounds{}; GetClientRect(control, &bounds);
    FillRect(dc, &bounds, settingsBackgroundBrush);
    const auto state = SendMessageW(control, BM_GETSTATE, 0, 0);
    const bool enabled = IsWindowEnabled(control) != FALSE;
    const bool pressed = (state & BST_PUSHED) != 0;
    const bool hover = GetPropW(control, L"ClippyHover") != nullptr;
    const auto type = GetWindowLongPtrW(control, GWL_STYLE) & BS_TYPEMASK;
    wchar_t caption[256]{}; GetWindowTextW(control, caption, 256);
    RECT textBounds = bounds;
    if (type == BS_AUTOCHECKBOX) {
        const int size = FloatScale(control, 16);
        RECT check{FloatScale(control, 1), (bounds.bottom - size) / 2,
            FloatScale(control, 1) + size, (bounds.bottom + size) / 2};
        const bool checked = (state & BST_CHECKED) != 0;
        Box(dc, check, checked ? TEAL : PANEL, FloatScale(control, 4));
        HBRUSH border = CreateSolidBrush(hover || GetFocus() == control ? TEAL : MUTED);
        FrameRect(dc, &check, border); DeleteObject(border);
        if (checked) {
            HPEN pen = CreatePen(PS_SOLID, FloatScale(control, 2), BG);
            auto previous = SelectObject(dc, pen);
            MoveToEx(dc, check.left + FloatScale(control, 3), check.top + size / 2, nullptr);
            LineTo(dc, check.left + FloatScale(control, 6), check.bottom - FloatScale(control, 4));
            LineTo(dc, check.right - FloatScale(control, 3), check.top + FloatScale(control, 4));
            SelectObject(dc, previous); DeleteObject(pen);
        }
        textBounds.left = check.right + FloatScale(control, 8);
        Text(dc, caption, textBounds, settingsFont, enabled ? INK : MUTED);
    } else {
        const bool primary = GetDlgCtrlID(control) == ID_SAVE;
        const COLORREF fill = primary ? (pressed ? RGB(49, 192, 159) : TEAL)
            : pressed ? RGB(24, 75, 64) : hover ? RGB(48, 64, 82) : PANEL;
        Box(dc, bounds, fill, FloatScale(control, 8));
        Text(dc, caption, textBounds, settingsFont, enabled ? (primary ? BG : INK) : MUTED,
            DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    DrawSettingsFocus(control, dc, textBounds);
}
LRESULT CALLBACK DarkButtonProc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(control, &paint);
        PaintDarkButton(control, dc); EndPaint(control, &paint); return 0;
    }
    if (message == WM_PRINTCLIENT) { PaintDarkButton(control, reinterpret_cast<HDC>(wp)); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_MOUSEMOVE && !GetPropW(control, L"ClippyHover")) {
        SetPropW(control, L"ClippyHover", reinterpret_cast<HANDLE>(1));
        TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, control, 0}; TrackMouseEvent(&tracking);
        InvalidateRect(control, nullptr, FALSE);
    }
    if (message == WM_MOUSELEAVE) { RemovePropW(control, L"ClippyHover"); InvalidateRect(control, nullptr, FALSE); }
    if (message == WM_NCDESTROY) {
        RemovePropW(control, L"ClippyHover"); RemoveWindowSubclass(control, DarkButtonProc, id);
    }
    const auto result = DefSubclassProc(control, message, wp, lp);
    if (message == BM_SETCHECK || message == BM_SETSTATE || message == BM_SETSTYLE ||
        message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_UPDATEUISTATE)
        InvalidateRect(control, nullptr, FALSE);
    return result;
}
void PaintDarkCombo(HWND control, HDC dc) {
    RECT bounds{}; GetClientRect(control, &bounds);
    FillRect(dc, &bounds, settingsPanelBrush);
    const int arrowWidth = GetSystemMetricsForDpi(SM_CXVSCROLL, GetDpiForWindow(control));
    const int selected = static_cast<int>(SendMessageW(control, CB_GETCURSEL, 0, 0));
    wchar_t text[256]{};
    if (selected != CB_ERR) SendMessageW(control, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text));
    RECT textBounds = bounds; textBounds.left += FloatScale(control, 8); textBounds.right -= arrowWidth;
    Text(dc, text, textBounds, settingsFont, IsWindowEnabled(control) ? INK : MUTED);
    const int centerX = bounds.right - arrowWidth / 2 - FloatScale(control, 1);
    const int centerY = bounds.bottom / 2;
    HPEN pen = CreatePen(PS_SOLID, FloatScale(control, 2), TEAL);
    auto previous = SelectObject(dc, pen);
    MoveToEx(dc, centerX - FloatScale(control, 4), centerY - FloatScale(control, 2), nullptr);
    LineTo(dc, centerX, centerY + FloatScale(control, 2));
    LineTo(dc, centerX + FloatScale(control, 4), centerY - FloatScale(control, 2));
    SelectObject(dc, previous); DeleteObject(pen);
    DrawSettingsFocus(control, dc, textBounds);
}
LRESULT CALLBACK DarkComboProc(HWND control, UINT message, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR) {
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; HDC dc = BeginPaint(control, &paint);
        PaintDarkCombo(control, dc); EndPaint(control, &paint); return 0;
    }
    if (message == WM_PRINTCLIENT) { PaintDarkCombo(control, reinterpret_cast<HDC>(wp)); return 0; }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_CTLCOLORLISTBOX) {
        HDC dc = reinterpret_cast<HDC>(wp); SetTextColor(dc, INK); SetBkColor(dc, PANEL);
        return reinterpret_cast<LRESULT>(settingsPanelBrush);
    }
    if (message == WM_NCDESTROY) RemoveWindowSubclass(control, DarkComboProc, id);
    const auto result = DefSubclassProc(control, message, wp, lp);
    if (message == CB_SETCURSEL || message == WM_SETFOCUS || message == WM_KILLFOCUS ||
        message == WM_ENABLE || message == WM_UPDATEUISTATE || message == WM_LBUTTONDOWN ||
        message == WM_LBUTTONUP || message == WM_KEYDOWN)
        InvalidateRect(control, nullptr, FALSE);
    return result;
}
HWND Control(HWND parent, const wchar_t* type, const wchar_t* text, DWORD style, int id, int x, int y, int w, int h) {
    int dpi = GetDpiForWindow(parent);
    HWND result = CreateWindowExW(0, type, text, WS_CHILD | WS_VISIBLE | style,
        MulDiv(x, dpi, 96), MulDiv(y, dpi, 96), MulDiv(w, dpi, 96), MulDiv(h, dpi, 96),
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), instance, nullptr);
    SendMessageW(result, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont ? settingsFont : uiFont), TRUE);
    SetWindowTheme(result, L"", L"");
    if (wcscmp(type, L"BUTTON") == 0) SetWindowSubclass(result, DarkButtonProc, 1, 0);
    if (wcscmp(type, L"COMBOBOX") == 0) SetWindowSubclass(result, DarkComboProc, 1, 0);
    return result;
}
void Label(HWND window, const wchar_t* text, int x, int y, int w = 490, int h = 22) { Control(window, L"STATIC", text, 0, -1, x, y, w, h); }
std::wstring ReadEdit(HWND window, int id) {
    wchar_t text[256]{}; GetDlgItemTextW(window, id, text, 256); return text;
}
void SetChecked(HWND window, int id, bool value) { SendDlgItemMessageW(window, id, BM_SETCHECK, value ? BST_CHECKED : BST_UNCHECKED, 0); }
bool Checked(HWND window, int id) { return SendDlgItemMessageW(window, id, BM_GETCHECK, 0, 0) == BST_CHECKED; }
int OpacitySliderValue(HWND window) {
    return std::clamp(static_cast<int>(SendDlgItemMessageW(window, ID_OPACITY, TBM_GETPOS, 0, 0)), 60, 100);
}
void ShowOpacitySliderValue(HWND window) {
    const auto label = std::to_wstring(OpacitySliderValue(window)) + L"%";
    SetDlgItemTextW(window, ID_OPACITY_VALUE, label.c_str());
}
void ResetOpacityPreview(HWND window) {
    SendDlgItemMessageW(window, ID_OPACITY, TBM_SETPOS, TRUE, settings.floatingOpacityPercent);
    ShowOpacitySliderValue(window);
    ApplyFloatingOpacity();
}
void CreateSettingsControls(HWND window) {
    int dpi = GetDpiForWindow(window);
    SetPropW(window, L"ClippyDpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(dpi)));
    settingsFont = CreateFontW(-MulDiv(13, dpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    Label(window, L"Capture shortcuts", 22, 16);
    Label(window, L"Add screenshot to draft", 22, 47, 240);
    Control(window, L"EDIT", HotkeyLabel(settings.paste).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_PASTE, 270, 44, 250, 27);
    Label(window, L"Add screenshot and send draft", 22, 82, 240);
    Control(window, L"EDIT", HotkeyLabel(settings.pasteSend).c_str(), WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, ID_SEND, 270, 79, 250, 27);
    Label(window, L"Examples: K, Shift+K, F8, Ctrl+Alt+K", 22, 114);
    Label(window, L"Ctrl+Alt+Space always arms / pauses the shortcuts.", 22, 139);
    Control(window, L"BUTTON", L"Show floating capture buttons", BS_AUTOCHECKBOX | WS_TABSTOP, ID_FLOAT, 22, 176, 490, 24);
    Control(window, L"BUTTON", L"Use 1–4 to select modes while capture is armed", BS_AUTOCHECKBOX | WS_TABSTOP, ID_NUMBERS, 22, 208, 490, 24);
    Label(window, L"After adding a screenshot", 22, 251, 240);
    HWND restore = Control(window, L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP, ID_RETURN, 270, 247, 250, 100);
    SendMessageW(restore, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Restore previous layout"));
    SendMessageW(restore, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"Minimize Chrome"));
    SendMessageW(restore, CB_SETCURSEL, settings.minimizeChrome ? 1 : 0, 0);
    SendMessageW(restore, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), MulDiv(23, dpi, 96));
    SendMessageW(restore, CB_SETITEMHEIGHT, 0, MulDiv(23, dpi, 96));
    Label(window, L"New ChatGPT tab load delay (ms)", 22, 290, 240);
    Control(window, L"EDIT", std::to_wstring(settings.loadDelayMs).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, ID_DELAY, 270, 286, 100, 27);
    Label(window, L"Upload readiness timeout (ms)", 22, 328, 240);
    Control(window, L"EDIT", std::to_wstring(settings.uploadTimeoutMs).c_str(), WS_BORDER | ES_NUMBER | WS_TABSTOP, ID_TIMEOUT, 270, 324, 100, 27);
    Label(window, L"Panel opacity (60–100%)", 22, 366, 240);
    HWND opacity = Control(window, TRACKBAR_CLASSW, L"Floating panel opacity", TBS_HORZ | TBS_NOTICKS | WS_TABSTOP,
        ID_OPACITY, 270, 359, 200, 34);
    SendMessageW(opacity, TBM_SETRANGE, TRUE, MAKELONG(60, 100));
    SendMessageW(opacity, TBM_SETLINESIZE, 0, 1); SendMessageW(opacity, TBM_SETPAGESIZE, 0, 5);
    Control(window, L"STATIC", L"", SS_RIGHT, ID_OPACITY_VALUE, 477, 366, 43, 22);
    ResetOpacityPreview(window);
    Label(window, L"Pause capture before typing: K and 1–4 are intercepted while armed.", 22, 407, 490, 43);
    Label(window, L"Mode 4 remembers your region. Press 4 again to redraw it.", 22, 452);
    Control(window, L"BUTTON", L"Arm / pause", BS_PUSHBUTTON | WS_TABSTOP, ID_ARM, 22, 491, 126, 32);
    Control(window, L"BUTTON", L"Save settings", BS_DEFPUSHBUTTON | WS_TABSTOP, ID_SAVE, 383, 491, 137, 32);
    Control(window, L"STATIC", L"", 0, ID_STATUS, 22, 534, 490, 46);
    SetChecked(window, ID_FLOAT, settings.floating); SetChecked(window, ID_NUMBERS, settings.modeKeys);
}
bool ParseNumber(const std::wstring& text, int min, int max, int& value) {
    wchar_t* end = nullptr; long parsed = wcstol(text.c_str(), &end, 10);
    if (text.empty() || !end || *end || parsed < min || parsed > max) return false;
    value = static_cast<int>(parsed); return true;
}
void SaveFromControls(HWND window) {
    if (busy) { SetDlgItemTextW(window, ID_STATUS, L"Wait for the current capture to finish."); return; }
    Settings candidate = settings; std::wstring error;
    if (!ParseHotkey(ReadEdit(window, ID_PASTE), candidate.paste, error) ||
        !ParseHotkey(ReadEdit(window, ID_SEND), candidate.pasteSend, error)) { SetDlgItemTextW(window, ID_STATUS, error.c_str()); return; }
    candidate.floating = Checked(window, ID_FLOAT); candidate.modeKeys = Checked(window, ID_NUMBERS);
    candidate.minimizeChrome = SendDlgItemMessageW(window, ID_RETURN, CB_GETCURSEL, 0, 0) == 1;
    if (!ValidateHotkeys(candidate, error)) { SetDlgItemTextW(window, ID_STATUS, error.c_str()); return; }
    if (!ParseNumber(ReadEdit(window, ID_DELAY), 0, 60000, candidate.loadDelayMs) ||
        !ParseNumber(ReadEdit(window, ID_TIMEOUT), 2000, 120000, candidate.uploadTimeoutMs)) {
        SetDlgItemTextW(window, ID_STATUS, L"Load delay must be 0–60000 ms; upload timeout 2000–120000 ms."); return;
    }
    candidate.floatingOpacityPercent = OpacitySliderValue(window);
    Settings previous = settings; settings = candidate;
    if (!RegisterCaptureKeys(error) || !SaveSettings(settings, error)) {
        settings = previous; std::wstring unused; RegisterCaptureKeys(unused);
        SetDlgItemTextW(window, ID_STATUS, error.c_str()); return;
    }
    ApplyFloatingOpacity();
    if (floating) ShowWindow(floating, settings.floating ? SW_SHOWNOACTIVATE : SW_HIDE);
    SetDlgItemTextW(window, ID_STATUS, L"Settings saved. Capture mode starts at 3 (both monitors) each launch.");
    Refresh();
}
LRESULT DrawOpacitySlider(NMCUSTOMDRAW& draw) {
    const HWND slider = draw.hdr.hwndFrom;
    if (draw.dwDrawStage == CDDS_PREPAINT) {
        RECT bounds{}; GetClientRect(slider, &bounds);
        FillRect(draw.hdc, &bounds, settingsBackgroundBrush);
        return CDRF_NOTIFYITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    if (draw.dwDrawStage == CDDS_ITEMPREPAINT) {
        if (draw.dwItemSpec == TBCD_CHANNEL) {
            Box(draw.hdc, draw.rc, PANEL, FloatScale(slider, 4));
            RECT filled = draw.rc;
            const int position = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
            filled.right = filled.left + MulDiv(std::clamp(position, 60, 100) - 60,
                draw.rc.right - draw.rc.left, 40);
            if (filled.right > filled.left) Box(draw.hdc, filled, TEAL, FloatScale(slider, 4));
            return CDRF_SKIPDEFAULT;
        }
        if (draw.dwItemSpec == TBCD_THUMB) {
            Box(draw.hdc, draw.rc, IsWindowEnabled(slider) ? TEAL : MUTED, FloatScale(slider, 6));
            return CDRF_SKIPDEFAULT;
        }
        if (draw.dwItemSpec == TBCD_TICS) return CDRF_SKIPDEFAULT;
    }
    if (draw.dwDrawStage == CDDS_POSTPAINT) {
        RECT bounds{}; GetClientRect(slider, &bounds); DrawSettingsFocus(slider, draw.hdc, bounds);
    }
    return CDRF_DODEFAULT;
}
void ApplySettingsTitleColors(HWND window) {
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    // Windows 11 supports explicit caption colors; older versions simply ignore
    // unsupported attributes and retain their standard non-client appearance.
    const COLORREF caption = BG, text = INK, border = PANEL;
    DwmSetWindowAttribute(window, DWMWA_CAPTION_COLOR, &caption, sizeof(caption));
    DwmSetWindowAttribute(window, DWMWA_TEXT_COLOR, &text, sizeof(text));
    DwmSetWindowAttribute(window, DWMWA_BORDER_COLOR, &border, sizeof(border));
}
LRESULT CALLBACK SettingsProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: ApplySettingsTitleColors(window); CreateSettingsControls(window); return 0;
    case WM_ERASEBKGND: {
        RECT bounds{}; GetClientRect(window, &bounds);
        FillRect(reinterpret_cast<HDC>(wp), &bounds, settingsBackgroundBrush); return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORLISTBOX:
    case WM_CTLCOLORBTN: {
        const HWND child = reinterpret_cast<HWND>(lp);
        const HDC dc = reinterpret_cast<HDC>(wp);
        const bool panel = msg == WM_CTLCOLOREDIT || msg == WM_CTLCOLORLISTBOX;
        const bool accent = GetDlgCtrlID(child) == ID_OPACITY_VALUE;
        SetTextColor(dc, accent ? TEAL : (panel ? INK : MUTED));
        SetBkColor(dc, panel ? PANEL : BG);
        return reinterpret_cast<LRESULT>(panel ? settingsPanelBrush : settingsBackgroundBrush);
    }
    case WM_MEASUREITEM: {
        auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
        if (item->CtlType == ODT_COMBOBOX && item->CtlID == ID_RETURN) {
            item->itemHeight = FloatScale(window, 23); return TRUE;
        }
        break;
    }
    case WM_DRAWITEM: {
        const auto* item = reinterpret_cast<const DRAWITEMSTRUCT*>(lp);
        if (item->CtlType == ODT_COMBOBOX && item->CtlID == ID_RETURN) {
            const bool selected = (item->itemState & ODS_SELECTED) != 0;
            HBRUSH brush = selected ? CreateSolidBrush(RGB(24, 75, 64)) : settingsPanelBrush;
            FillRect(item->hDC, &item->rcItem, brush);
            if (selected) DeleteObject(brush);
            wchar_t text[256]{};
            if (item->itemID != static_cast<UINT>(-1))
                SendMessageW(item->hwndItem, CB_GETLBTEXT, item->itemID, reinterpret_cast<LPARAM>(text));
            RECT bounds = item->rcItem; bounds.left += FloatScale(window, 8);
            Text(item->hDC, text, bounds, settingsFont, selected ? TEAL : INK);
            if (item->itemState & ODS_FOCUS) {
                RECT focus = item->rcItem; InflateRect(&focus, -FloatScale(window, 2), -FloatScale(window, 2));
                SetTextColor(item->hDC, TEAL); SetBkColor(item->hDC, PANEL); DrawFocusRect(item->hDC, &focus);
            }
            return TRUE;
        }
        break;
    }
    case WM_NOTIFY: {
        auto* notification = reinterpret_cast<NMHDR*>(lp);
        if (notification->idFrom == ID_OPACITY && notification->code == NM_CUSTOMDRAW)
            return DrawOpacitySlider(*reinterpret_cast<NMCUSTOMDRAW*>(lp));
        break;
    }
    case WM_HSCROLL:
        if (reinterpret_cast<HWND>(lp) == GetDlgItem(window, ID_OPACITY)) {
            ShowOpacitySliderValue(window); ApplyFloatingOpacity(OpacitySliderValue(window)); return 0;
        }
        break;
    case WM_SHOWWINDOW: ResetOpacityPreview(window); break;
    case WM_COMMAND:
        if (LOWORD(wp) == ID_SAVE) SaveFromControls(window);
        if (LOWORD(wp) == ID_ARM) SetArmed(!armed);
        return 0;
    case WM_CLOSE: ShowWindow(window, SW_HIDE); return 0;
    case WM_DPICHANGED: {
        int oldDpi = static_cast<int>(reinterpret_cast<UINT_PTR>(GetPropW(window, L"ClippyDpi")));
        int newDpi = HIWORD(wp); if (!oldDpi) oldDpi = 96;
        HFONT oldFont = settingsFont;
        settingsFont = CreateFontW(-MulDiv(13, newDpi, 96), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        for (HWND child = GetWindow(window, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
            RECT rect{}; GetWindowRect(child, &rect); MapWindowPoints(nullptr, window, reinterpret_cast<POINT*>(&rect), 2);
            SetWindowPos(child, nullptr, MulDiv(rect.left, newDpi, oldDpi), MulDiv(rect.top, newDpi, oldDpi),
                MulDiv(rect.right - rect.left, newDpi, oldDpi), MulDiv(rect.bottom - rect.top, newDpi, oldDpi), SWP_NOZORDER | SWP_NOACTIVATE);
            SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(settingsFont), TRUE);
        }
        if (oldFont) DeleteObject(oldFont);
        SendDlgItemMessageW(window, ID_RETURN, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), MulDiv(23, newDpi, 96));
        SendDlgItemMessageW(window, ID_RETURN, CB_SETITEMHEIGHT, 0, MulDiv(23, newDpi, 96));
        SetPropW(window, L"ClippyDpi", reinterpret_cast<HANDLE>(static_cast<UINT_PTR>(newDpi)));
        const RECT& rect = *reinterpret_cast<const RECT*>(lp);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top, SWP_NOZORDER | SWP_NOACTIVATE);
        return 0;
    }
    case WM_DESTROY: ApplyFloatingOpacity(); if (settingsFont) DeleteObject(settingsFont); settingsFont = nullptr; RemovePropW(window, L"ClippyDpi"); settingsWindow = nullptr; return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
void OpenSettings() {
    if (!settingsWindow) {
        DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
        UINT dpi = GetDpiForSystem(); RECT rect{0, 0, MulDiv(540, dpi, 96), MulDiv(590, dpi, 96)};
        AdjustWindowRectExForDpi(&rect, style, FALSE, 0, dpi);
        settingsWindow = CreateWindowExW(0, L"ClippySettings", L"Clippy settings", style,
            CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
            nullptr, nullptr, instance, nullptr);
    }
    ResetOpacityPreview(settingsWindow);
    ShowWindow(settingsWindow, SW_SHOW); SetForegroundWindow(settingsWindow);
}
void TrayMenu() {
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (armed ? MF_CHECKED : 0), CMD_TOGGLE, L"Capture armed  ·  Ctrl+Alt+Space");
    AppendMenuW(menu, MF_STRING | (settings.floating ? MF_CHECKED : 0), CMD_FLOAT, L"Floating buttons");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    for (int i = 1; i <= 4; ++i) {
        const wchar_t* labels[] = {L"1 · Monitor 1 (primary)", L"2 · Monitor 2", L"3 · Both monitors", L"4 · Drag a region"};
        AppendMenuW(menu, MF_STRING | (static_cast<int>(settings.mode) == i ? MF_CHECKED : 0), 110 + i, labels[i - 1]);
    }
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, CMD_SETTINGS, L"Settings…");
    AppendMenuW(menu, MF_STRING, CMD_EXIT, L"Exit Clippy");
    POINT cursor{}; GetCursorPos(&cursor); SetForegroundWindow(controller);
    int selected = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, cursor.x, cursor.y, 0, controller, nullptr);
    DestroyMenu(menu); if (selected) SendMessageW(controller, WM_COMMAND, selected, 0);
    PostMessageW(controller, WM_NULL, 0, 0);
}
void SetFloatingVisible(bool visible) {
    settings.floating = visible;
    hoveredHeaderButton = 0;
    trackingFloatingMouse = false;
    if (floatingTooltips) SendMessageW(floatingTooltips, TTM_POP, 0, 0);
    if (floating) ShowWindow(floating, visible ? SW_SHOWNOACTIVATE : SW_HIDE);
    if (settingsWindow) SetChecked(settingsWindow, ID_FLOAT, visible);
    std::wstring error;
    if (!SaveSettings(settings, error)) Notify(L"Clippy settings", error);
    Refresh();
}
LRESULT CALLBACK FloatProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_PAINT: PaintFloat(window); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
    case WM_MOUSEMOVE: {
        POINT point{MulDiv(static_cast<short>(LOWORD(lp)), 96, GetDpiForWindow(window)),
            MulDiv(static_cast<short>(HIWORD(lp)), 96, GetDpiForWindow(window))};
        const int hovered = HeaderButtonAt(point);
        if (hovered != hoveredHeaderButton) { hoveredHeaderButton = hovered; Refresh(); }
        if (!trackingFloatingMouse) {
            TRACKMOUSEEVENT tracking{sizeof(tracking), TME_LEAVE, window, 0};
            trackingFloatingMouse = TrackMouseEvent(&tracking) != FALSE;
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        hoveredHeaderButton = 0; trackingFloatingMouse = false; Refresh(); return 0;
    case WM_LBUTTONDOWN: {
        int x = MulDiv(static_cast<short>(LOWORD(lp)), 96, GetDpiForWindow(window));
        int y = MulDiv(static_cast<short>(HIWORD(lp)), 96, GetDpiForWindow(window));
        const int headerButton = HeaderButtonAt(POINT{x, y});
        if (headerButton == 2) { SendMessageW(controller, WM_COMMAND, CMD_MINIMIZE, 0); return 0; }
        if (headerButton == 3) {
            cancelRequested.store(true);
            PostMessageW(controller, WM_COMMAND, CMD_EXIT, 0); return 0;
        }
        if (headerButton == 1) { SetArmed(!armed); return 0; }
        if (y <= 36) { ReleaseCapture(); SendMessageW(window, WM_NCLBUTTONDOWN, HTCAPTION, 0); return 0; }
        if (y >= 42 && y <= 110) { StartCapture(x >= 140); return 0; }
        if (y >= 119 && y <= 146 && x >= 12 && x < 268) { SelectMode(std::clamp((x - 12) / 65 + 1, 1, 4)); return 0; }
        if (y >= 180 && x >= 195) OpenSettings();
        return 0;
    }
    case WM_RBUTTONUP: TrayMenu(); return 0;
    case WM_TIMER: pulse = 0; KillTimer(window, 1); Refresh(); return 0;
    case WM_EXITSIZEMOVE: {
        RECT rect{}; GetWindowRect(window, &rect); settings.floatingX = rect.left; settings.floatingY = rect.top;
        std::wstring error; SaveSettings(settings, error); return 0;
    }
    case WM_DPICHANGED: {
        const auto* proposed = reinterpret_cast<const RECT*>(lp);
        SetWindowPos(window, nullptr, proposed->left, proposed->top, proposed->right - proposed->left,
            proposed->bottom - proposed->top, SWP_NOZORDER | SWP_NOACTIVATE);
        UpdateFloatingTooltips();
        if (floatingTooltips) SendMessageW(floatingTooltips, TTM_SETMAXTIPWIDTH, 0, FloatScale(window, 300));
        return 0;
    }
    case WM_CLOSE:
        cancelRequested.store(true); PostMessageW(controller, WM_COMMAND, CMD_EXIT, 0); return 0;
    case WM_DESTROY:
        if (floatingTooltips && IsWindow(floatingTooltips)) DestroyWindow(floatingTooltips);
        floatingTooltips = nullptr; floating = nullptr; return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
void Shutdown() {
    if (exiting) return;
    exiting = true; UnregisterCaptureKeys(); UnregisterHotKey(controller, HK_TOGGLE);
    cancelRequested.store(true);
    if (deliveryThread.joinable()) {
        SetStatus(L"Finishing the current capture…"); deliveryThread.join();
    }
    if (foregroundHook) UnhookWinEvent(foregroundHook);
    NOTIFYICONDATAW data{}; data.cbSize = sizeof(data); data.hWnd = controller; data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
    DestroyWindow(floating); if (settingsWindow) DestroyWindow(settingsWindow);
    PostQuitMessage(0);
}
LRESULT CALLBACK ControllerProc(HWND window, UINT msg, WPARAM wp, LPARAM lp) {
    static UINT taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    if (msg == taskbarCreated) {
        NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon); icon.hWnd = window; icon.uID = 1;
        icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; icon.hIcon = appIcon; icon.uCallbackMessage = WM_TRAY;
        wcscpy(icon.szTip, L"Clippy · screenshot to ChatGPT"); Shell_NotifyIconW(NIM_ADD, &icon); return 0;
    }
    switch (msg) {
    case WM_HOTKEY:
        if (wp == HK_TOGGLE) { if (!suppressToggle) SetArmed(!armed); }
        else if (wp == HK_PASTE && armed) StartCapture(false);
        else if (wp == HK_SEND && armed) StartCapture(true);
        else if (wp > HK_MODE && wp <= HK_MODE + 4 && armed) SelectMode(static_cast<int>(wp) - HK_MODE);
        return 0;
    case WM_STATUS: {
        std::unique_ptr<std::wstring> value(reinterpret_cast<std::wstring*>(lp));
        SetStatus(*value); return 0;
    }
    case WM_DONE: {
        std::unique_ptr<DeliveryResult> value(reinterpret_cast<DeliveryResult*>(lp));
        if (deliveryThread.joinable()) deliveryThread.join();
        busy = false; std::wstring error;
        if (restoreSettings && settingsWindow && !exiting) ShowWindow(settingsWindow, SW_SHOWNOACTIVATE);
        restoreSettings = false;
        if (!RegisterCaptureKeys(error)) { armed = false; SetStatus(error); }
        else SetStatus(value->message);
        if (!value->success) Notify(L"Clippy needs attention", value->message);
        return 0;
    }
    case WM_TRAY:
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) TrayMenu();
        else if (LOWORD(lp) == WM_LBUTTONDBLCLK) OpenSettings();
        else if (LOWORD(lp) == WM_LBUTTONUP) {
            SetFloatingVisible(true);
        }
        return 0;
    case WM_COMMAND:
        if (wp == CMD_SETTINGS) OpenSettings();
        else if (wp == CMD_TOGGLE) SetArmed(!armed);
        else if (wp == CMD_FLOAT) {
            SetFloatingVisible(!settings.floating);
        }
        else if (wp == CMD_MINIMIZE) SetFloatingVisible(false);
        else if (wp == CMD_EXIT) Shutdown();
        else if (wp > 110 && wp <= 114) SelectMode(static_cast<int>(wp) - 110);
        return 0;
    case WM_CLOSE: Shutdown(); return 0;
    }
    return DefWindowProcW(window, msg, wp, lp);
}
void RegisterClass(const wchar_t* name, WNDPROC procedure, HBRUSH background = nullptr) {
    WNDCLASSEXW info{}; info.cbSize = sizeof(info); info.hInstance = instance; info.lpfnWndProc = procedure;
    info.lpszClassName = name; info.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    info.hIcon = appIcon; info.hIconSm = appIcon; info.hbrBackground = background;
    RegisterClassExW(&info);
}
}

int WINAPI wWinMain(HINSTANCE app, HINSTANCE, PWSTR command, int) {
    instance = app;
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (wcsstr(command, L"--self-test")) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        FILE* output = _wfreopen(L"CONOUT$", L"w", stdout); (void)output;
        return RunSelfTests();
    }
    if (wcsstr(command, L"--probe-chrome")) {
        AttachConsole(ATTACH_PARENT_PROCESS);
        FILE* output = _wfreopen(L"CONOUT$", L"w", stdout); (void)output;
        std::wstring report; bool result = ProbeChrome(report);
        std::wprintf(L"%ls\n", report.c_str()); return result ? 0 : 1;
    }
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\Clippy.Desktop.SingleInstance");
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"ClippyController", L"Clippy");
        DWORD process = 0; GetWindowThreadProcessId(existing, &process);
        if (process) AllowSetForegroundWindow(process);
        if (existing) PostMessageW(existing, WM_COMMAND, CMD_SETTINGS, 0);
        CloseHandle(mutex); return 0;
    }
    settings = LoadSettings(); settings.mode = CaptureMode::AllMonitors; settings.numbersCapture = false;
    INITCOMMONCONTROLSEX commonControls{sizeof(commonControls), ICC_WIN95_CLASSES | ICC_BAR_CLASSES};
    InitCommonControlsEx(&commonControls);
    appIcon = LoadIconW(nullptr, IDI_APPLICATION);
    uiFont = CreateFontW(-13, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    titleFont = CreateFontW(-19, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    keyFont = CreateFontW(-21, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    RegisterClass(L"ClippyController", ControllerProc);
    RegisterClass(L"ClippyFloat", FloatProc);
    settingsBackgroundBrush = CreateSolidBrush(BG); settingsPanelBrush = CreateSolidBrush(PANEL);
    RegisterClass(L"ClippySettings", SettingsProc, settingsBackgroundBrush);
    controller = CreateWindowExW(WS_EX_TOOLWINDOW, L"ClippyController", L"Clippy", WS_POPUP,
        0, 0, 0, 0, nullptr, nullptr, instance, nullptr);
    RECT work{}; SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    int x = settings.floatingX != -1 ? settings.floatingX : work.right - 300;
    int y = settings.floatingY != -1 ? settings.floatingY : work.bottom - 230;
    RECT proposed{x, y, x + 280, y + 210};
    if (!MonitorFromRect(&proposed, MONITOR_DEFAULTTONULL)) { x = work.right - 300; y = work.bottom - 230; }
    floating = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_LAYERED,
        L"ClippyFloat", L"Clippy capture buttons", WS_POPUP, x, y, 280, 210,
        nullptr, nullptr, instance, nullptr);
    if (!controller || !floating) { CloseHandle(mutex); return 1; }
    SetWindowPos(floating, nullptr, x, y, FloatScale(floating, 280), FloatScale(floating, 210), SWP_NOACTIVATE | SWP_NOZORDER);
    ApplyFloatingOpacity();
    CreateFloatingTooltips();
    int corners = 2; DwmSetWindowAttribute(floating, 33, &corners, sizeof(corners));
    NOTIFYICONDATAW icon{}; icon.cbSize = sizeof(icon); icon.hWnd = controller; icon.uID = 1;
    icon.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP; icon.hIcon = appIcon; icon.uCallbackMessage = WM_TRAY;
    wcscpy(icon.szTip, L"Clippy · screenshot to ChatGPT"); Shell_NotifyIconW(NIM_ADD, &icon);
    if (!RegisterHotKey(controller, HK_TOGGLE, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, VK_SPACE))
        SetStatus(L"Toggle shortcut in use · click Paused to arm");
    lastExternalWindow = GetForegroundWindow();
    foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr,
        ForegroundEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    if (settings.floating) ShowWindow(floating, SW_SHOWNOACTIVATE);
    if (wcsstr(command, L"--settings")) OpenSettings();
    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (settingsWindow && IsWindowVisible(settingsWindow) && IsDialogMessageW(settingsWindow, &message)) continue;
        TranslateMessage(&message); DispatchMessageW(&message);
    }
    // Drain heap-backed notifications that may have arrived during shutdown.
    while (PeekMessageW(&message, controller, WM_STATUS, WM_DONE, PM_REMOVE)) {
        if (message.message == WM_STATUS) delete reinterpret_cast<std::wstring*>(message.lParam);
        if (message.message == WM_DONE) delete reinterpret_cast<DeliveryResult*>(message.lParam);
    }
    DestroyWindow(controller); DeleteObject(uiFont); DeleteObject(titleFont); DeleteObject(keyFont);
    DeleteObject(settingsBackgroundBrush); DeleteObject(settingsPanelBrush); CloseHandle(mutex);
    return 0;
}
