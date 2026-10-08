#include "clippy.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace clippy {
namespace {

constexpr std::uint64_t kMaximumDibBytes = 512ull * 1024 * 1024;
constexpr wchar_t kRegionClass[] = L"Clippy.RegionSelection";

std::wstring WindowsError(const wchar_t* operation, DWORD code = GetLastError()) {
    wchar_t* description = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
        code, 0, reinterpret_cast<LPWSTR>(&description), 0, nullptr);
    std::wstring message(operation);
    if (length && description) {
        std::wstring detail(description, length);
        while (!detail.empty() && (detail.back() == L'\r' || detail.back() == L'\n' ||
                                  detail.back() == L' ')) detail.pop_back();
        message += L": " + detail;
    } else {
        message += L" (Windows error " + std::to_wstring(code) + L")";
    }
    if (description) LocalFree(description);
    return message;
}

struct ScreenDC {
    HDC value = GetDC(nullptr);
    ~ScreenDC() { if (value) ReleaseDC(nullptr, value); }
};

// A selected bitmap must be removed from its DC before either object is deleted.
struct BitmapDC {
    HDC dc = nullptr;
    HBITMAP bitmap = nullptr;
    HGDIOBJ original = nullptr;
    BitmapDC() = default;
    BitmapDC(const BitmapDC&) = delete;
    BitmapDC& operator=(const BitmapDC&) = delete;
    ~BitmapDC() {
        if (dc && original && original != HGDI_ERROR) SelectObject(dc, original);
        if (bitmap) DeleteObject(bitmap);
        if (dc) DeleteDC(dc);
    }
    bool Create(HDC source, const BITMAPINFO& info, void** pixels) {
        dc = CreateCompatibleDC(source);
        if (!dc) return false;
        bitmap = CreateDIBSection(source, &info, DIB_RGB_COLORS, pixels, nullptr, 0);
        if (!bitmap || !*pixels) return false;
        original = SelectObject(dc, bitmap);
        return original && original != HGDI_ERROR;
    }
};

bool DibSize(std::int64_t width, std::int64_t height, std::uint64_t& stride,
             std::uint64_t& bytes) {
    if (width <= 0 || height <= 0 || width > std::numeric_limits<int>::max() ||
        height > std::numeric_limits<int>::max()) return false;
    stride = (static_cast<std::uint64_t>(width) * 3 + 3) & ~3ull;
    bytes = stride * static_cast<std::uint64_t>(height);
    return bytes <= std::numeric_limits<DWORD>::max() &&
           bytes + sizeof(BITMAPINFOHEADER) <= kMaximumDibBytes &&
           bytes + sizeof(BITMAPINFOHEADER) <= std::numeric_limits<size_t>::max();
}

BOOL CALLBACK CollectMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM argument) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) return TRUE;
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    std::wstring name(info.szDevice);
    if (EnumDisplayDevicesW(info.szDevice, 0, &device, 0) && device.DeviceString[0]) {
        name = std::wstring(device.DeviceString) + L" [" + info.szDevice + L"]";
    }
    auto& monitors = *reinterpret_cast<std::vector<Monitor>*>(argument);
    monitors.push_back({info.rcMonitor, std::move(name),
                        (info.dwFlags & MONITORINFOF_PRIMARY) != 0});
    return TRUE;
}

struct RegionState {
    RECT desktop{};
    RECT selection{}; // Coordinates relative to the full virtual desktop window.
    POINT anchor{};
    POINT instructionOrigin{};
    bool dragging = false;
    bool done = false;
    bool accepted = false;
    BitmapDC frozen;
    BitmapDC dimmed;
    HFONT font = nullptr;
    ~RegionState() { if (font) DeleteObject(font); }
};

POINT CursorInRegion(const RegionState& state) {
    POINT point{};
    GetCursorPos(&point);
    point.x = std::clamp(point.x, state.desktop.left, state.desktop.right);
    point.y = std::clamp(point.y, state.desktop.top, state.desktop.bottom);
    point.x -= state.desktop.left;
    point.y -= state.desktop.top;
    return point;
}

void UpdateRegion(RegionState& state) {
    const POINT point = CursorInRegion(state);
    state.selection = {std::min(state.anchor.x, point.x),
                       std::min(state.anchor.y, point.y),
                       std::max(state.anchor.x, point.x),
                       std::max(state.anchor.y, point.y)};
}

bool HasRegion(const RegionState& state) {
    return state.selection.right > state.selection.left &&
           state.selection.bottom > state.selection.top;
}

void FinishRegion(HWND window, RegionState& state, bool accepted) {
    // Set this before releasing capture: WM_CAPTURECHANGED can arrive immediately.
    state.done = true;
    state.accepted = accepted;
    state.dragging = false;
    if (GetCapture() == window) ReleaseCapture();
}

void PaintRegion(HWND window, RegionState& state) {
    PAINTSTRUCT paint{};
    HDC target = BeginPaint(window, &paint);
    if (!target) { EndPaint(window, &paint); return; }
    const int width = state.desktop.right - state.desktop.left;
    const int height = state.desktop.bottom - state.desktop.top;
    BitBlt(target, 0, 0, width, height, state.dimmed.dc, 0, 0, SRCCOPY);
    const bool selection = HasRegion(state);
    if (selection) {
        const RECT& rect = state.selection;
        BitBlt(target, rect.left, rect.top, rect.right - rect.left,
               rect.bottom - rect.top, state.frozen.dc, rect.left, rect.top, SRCCOPY);
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(86, 185, 255));
        HGDIOBJ oldPen = pen ? SelectObject(target, pen) : nullptr;
        HGDIOBJ oldBrush = SelectObject(target, GetStockObject(NULL_BRUSH));
        Rectangle(target, rect.left, rect.top, rect.right, rect.bottom);
        if (oldBrush) SelectObject(target, oldBrush);
        if (oldPen) SelectObject(target, oldPen);
        if (pen) DeleteObject(pen);
    }
    HGDIOBJ oldFont = state.font ? SelectObject(target, state.font) : nullptr;
    SetTextColor(target, RGB(255, 255, 255));
    SetBkColor(target, RGB(24, 28, 36));
    SetBkMode(target, OPAQUE);
    const std::wstring instruction = L"Drag to capture a region  |  Esc to cancel";
    const int panelX = state.instructionOrigin.x;
    const int panelY = state.instructionOrigin.y;
    SIZE textSize{};
    GetTextExtentPoint32W(target, instruction.c_str(), static_cast<int>(instruction.size()), &textSize);
    RECT panel{panelX, panelY, panelX + textSize.cx + 28, panelY + textSize.cy + 20};
    ExtTextOutW(target, panelX + 14, panelY + 10, ETO_OPAQUE, &panel,
                instruction.c_str(), static_cast<UINT>(instruction.size()), nullptr);
    if (selection) {
        const std::wstring dimensions = std::to_wstring(state.selection.right - state.selection.left) +
            L" \u00d7 " + std::to_wstring(state.selection.bottom - state.selection.top) + L" px";
        GetTextExtentPoint32W(target, dimensions.c_str(), static_cast<int>(dimensions.size()), &textSize);
        int x = state.selection.left;
        int y = state.selection.top - textSize.cy - 16;
        if (y < 0) y = state.selection.top + 8;
        x = std::clamp(x, 0, std::max(0, width - static_cast<int>(textSize.cx) - 20));
        y = std::clamp(y, 0, std::max(0, height - static_cast<int>(textSize.cy) - 16));
        RECT label{x, y, x + textSize.cx + 20, y + textSize.cy + 16};
        ExtTextOutW(target, x + 10, y + 8, ETO_OPAQUE, &label,
                    dimensions.c_str(), static_cast<UINT>(dimensions.size()), nullptr);
    }
    if (oldFont) SelectObject(target, oldFont);
    EndPaint(window, &paint);
}

LRESULT CALLBACK RegionWindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto* state = reinterpret_cast<RegionState*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = static_cast<RegionState*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (!state) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_ERASEBKGND: return 1;
    case WM_PAINT: PaintRegion(window, *state); return 0;
    case WM_LBUTTONDOWN:
        if (!state->done) {
            state->anchor = CursorInRegion(*state);
            state->selection = {state->anchor.x, state->anchor.y, state->anchor.x, state->anchor.y};
            state->dragging = true;
            SetCapture(window);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_MOUSEMOVE:
        if (state->dragging) {
            UpdateRegion(*state);
            InvalidateRect(window, nullptr, FALSE);
        }
        return 0;
    case WM_LBUTTONUP:
        if (state->dragging) {
            UpdateRegion(*state);
            if (HasRegion(*state)) {
                FinishRegion(window, *state, true);
            } else {
                state->dragging = false;
                ReleaseCapture();
                InvalidateRect(window, nullptr, FALSE);
            }
        }
        return 0;
    case WM_KEYDOWN:
        if (wParam == VK_ESCAPE) FinishRegion(window, *state, false);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_CLOSE:
        FinishRegion(window, *state, false);
        return 0;
    case WM_CAPTURECHANGED:
        if (state->dragging && reinterpret_cast<HWND>(lParam) != window)
            FinishRegion(window, *state, false);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wParam) == WA_INACTIVE && !state->done)
            FinishRegion(window, *state, false);
        return 0;
    case WM_DPICHANGED:
        // This window spans the virtual desktop; keep its physical bounds unchanged.
        return 0;
    case WM_DESTROY:
        state->done = true;
        return 0; // The application's outer message loop owns WM_QUIT.
    default:
        return DefWindowProcW(window, message, wParam, lParam);
    }
}

void FlushDesktopComposition() {
    // Avoid requiring an extra import library for this optional synchronization.
    HMODULE module = LoadLibraryW(L"dwmapi.dll");
    if (module) {
        using FlushFunction = HRESULT (WINAPI*)();
        auto flush = reinterpret_cast<FlushFunction>(GetProcAddress(module, "DwmFlush"));
        if (flush) flush();
        FreeLibrary(module);
    }
}

} // namespace

std::vector<Monitor> GetMonitors() {
    std::vector<Monitor> monitors;
    EnumDisplayMonitors(nullptr, nullptr, CollectMonitor, reinterpret_cast<LPARAM>(&monitors));
    std::sort(monitors.begin(), monitors.end(), [](const Monitor& a, const Monitor& b) {
        if (a.primary != b.primary) return a.primary;
        if (a.bounds.left != b.bounds.left) return a.bounds.left < b.bounds.left;
        if (a.bounds.top != b.bounds.top) return a.bounds.top < b.bounds.top;
        return a.name < b.name;
    });
    return monitors;
}

RECT UnionMonitors(const std::vector<Monitor>& monitors) {
    RECT result{};
    bool found = false;
    for (const auto& monitor : monitors) {
        const RECT& bounds = monitor.bounds;
        if (bounds.right <= bounds.left || bounds.bottom <= bounds.top) continue;
        if (!found) {
            result = bounds;
            found = true;
        } else {
            result.left = std::min(result.left, bounds.left);
            result.top = std::min(result.top, bounds.top);
            result.right = std::max(result.right, bounds.right);
            result.bottom = std::max(result.bottom, bounds.bottom);
        }
    }
    return result;
}

bool CaptureRect(const RECT& rect, Screenshot& shot, std::wstring& error) {
    error.clear();
    const std::int64_t width = static_cast<std::int64_t>(rect.right) - rect.left;
    const std::int64_t height = static_cast<std::int64_t>(rect.bottom) - rect.top;
    std::uint64_t stride = 0, bytes = 0;
    if (!DibSize(width, height, stride, bytes)) {
        error = L"The capture area is empty or too large (maximum screenshot size is 512 MiB).";
        return false;
    }
    const RECT desktop = UnionMonitors(GetMonitors());
    if (rect.left < desktop.left || rect.top < desktop.top ||
        rect.right > desktop.right || rect.bottom > desktop.bottom) {
        error = L"The capture area lies outside the connected displays.";
        return false;
    }
    ScreenDC screen;
    if (!screen.value) {
        error = WindowsError(L"Could not access the screen");
        return false;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = static_cast<LONG>(width);
    info.bmiHeader.biHeight = static_cast<LONG>(height);
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 24;
    info.bmiHeader.biCompression = BI_RGB;
    info.bmiHeader.biSizeImage = static_cast<DWORD>(bytes);
    BitmapDC target;
    void* pixels = nullptr;
    if (!target.Create(screen.value, info, &pixels)) {
        error = WindowsError(L"Could not allocate the screenshot bitmap");
        return false;
    }
    // BitBlt does not promise to initialize scanline padding.
    std::memset(pixels, 0, static_cast<size_t>(bytes));
    if (!BitBlt(target.dc, 0, 0, static_cast<int>(width), static_cast<int>(height),
                screen.value, rect.left, rect.top, SRCCOPY | CAPTUREBLT)) {
        error = WindowsError(L"Could not capture the screen");
        return false;
    }
    // DIBSection memory is read directly only after GDI has completed its writes.
    GdiFlush();
    try {
        Screenshot result;
        result.width = static_cast<int>(width);
        result.height = static_cast<int>(height);
        result.dib.resize(sizeof(BITMAPINFOHEADER) + static_cast<size_t>(bytes));
        std::memcpy(result.dib.data(), &info.bmiHeader, sizeof(BITMAPINFOHEADER));
        std::memcpy(result.dib.data() + sizeof(BITMAPINFOHEADER), pixels, static_cast<size_t>(bytes));
        shot = std::move(result);
    } catch (const std::bad_alloc&) {
        error = L"There is not enough memory to store the screenshot.";
        return false;
    }
    return true;
}

bool SetScreenshotClipboard(HWND owner, const Screenshot& shot, std::wstring& error) {
    error.clear();
    BITMAPINFOHEADER header{};
    std::uint64_t stride = 0, bytes = 0;
    if (shot.dib.size() < sizeof(header)) {
        error = L"The screenshot contains no bitmap data.";
        return false;
    }
    std::memcpy(&header, shot.dib.data(), sizeof(header));
    if (header.biSize != sizeof(header) || header.biWidth != shot.width ||
        header.biHeight != shot.height || header.biPlanes != 1 ||
        header.biBitCount != 24 || header.biCompression != BI_RGB ||
        !DibSize(shot.width, shot.height, stride, bytes) ||
        shot.dib.size() != sizeof(header) + static_cast<size_t>(bytes)) {
        error = L"The screenshot bitmap is invalid.";
        return false;
    }
    HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, shot.dib.size());
    if (!data) {
        error = WindowsError(L"Could not allocate clipboard memory");
        return false;
    }
    void* destination = GlobalLock(data);
    if (!destination) {
        error = WindowsError(L"Could not access clipboard memory");
        GlobalFree(data);
        return false;
    }
    std::memcpy(destination, shot.dib.data(), shot.dib.size());
    GlobalUnlock(data);
    bool opened = false;
    for (int attempt = 0; attempt < 15; ++attempt) {
        if (OpenClipboard(owner)) {
            opened = true;
            break;
        }
        if (attempt != 14) Sleep(20);
    }
    if (!opened) {
        error = WindowsError(L"The clipboard is busy; try the capture again");
        GlobalFree(data);
        return false;
    }
    bool success = false;
    if (!EmptyClipboard()) {
        error = WindowsError(L"Could not clear the clipboard");
    } else if (!SetClipboardData(CF_DIB, data)) {
        error = WindowsError(L"Could not put the screenshot on the clipboard");
    } else {
        success = true; // Windows now owns this HGLOBAL and preserves it for another paste.
    }
    CloseClipboard();
    if (!success) GlobalFree(data);
    return success;
}

bool SelectRegion(HINSTANCE instance, HWND owner, RECT& selected) {
    RegionState state;
    const auto monitors = GetMonitors();
    state.desktop = UnionMonitors(monitors);
    const std::int64_t width64 = static_cast<std::int64_t>(state.desktop.right) - state.desktop.left;
    const std::int64_t height64 = static_cast<std::int64_t>(state.desktop.bottom) - state.desktop.top;
    if (width64 <= 0 || height64 <= 0 || width64 > std::numeric_limits<int>::max() ||
        height64 > std::numeric_limits<int>::max() ||
        static_cast<std::uint64_t>(width64) * static_cast<std::uint64_t>(height64) > kMaximumDibBytes / 8)
        return false;
    const int width = static_cast<int>(width64), height = static_cast<int>(height64);
    ScreenDC screen;
    if (!screen.value) return false;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* frozenPixels = nullptr;
    void* dimmedPixels = nullptr;
    if (!state.frozen.Create(screen.value, info, &frozenPixels) ||
        !state.dimmed.Create(screen.value, info, &dimmedPixels)) return false;
    if (!BitBlt(state.frozen.dc, 0, 0, width, height, screen.value,
                state.desktop.left, state.desktop.top, SRCCOPY | CAPTUREBLT)) return false;
    GdiFlush();
    const auto* original = static_cast<const std::uint8_t*>(frozenPixels);
    auto* dimmed = static_cast<std::uint8_t*>(dimmedPixels);
    const size_t pixelCount = static_cast<size_t>(width) * height;
    for (size_t pixel = 0; pixel < pixelCount; ++pixel) {
        const size_t offset = pixel * 4;
        dimmed[offset] = static_cast<std::uint8_t>(original[offset] * 45 / 100);
        dimmed[offset + 1] = static_cast<std::uint8_t>(original[offset + 1] * 45 / 100);
        dimmed[offset + 2] = static_cast<std::uint8_t>(original[offset + 2] * 45 / 100);
        dimmed[offset + 3] = 0;
    }
    WNDCLASSEXW existing{};
    existing.cbSize = sizeof(existing);
    if (!GetClassInfoExW(instance, kRegionClass, &existing)) {
        WNDCLASSEXW windowClass{};
        windowClass.cbSize = sizeof(windowClass);
        windowClass.hInstance = instance;
        windowClass.lpfnWndProc = RegionWindowProc;
        windowClass.hCursor = LoadCursorW(nullptr, IDC_CROSS);
        windowClass.lpszClassName = kRegionClass;
        if (!RegisterClassExW(&windowClass)) return false;
    }
    state.font = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
        DEFAULT_PITCH | FF_DONTCARE, L"Segoe UI");
    if (!monitors.empty()) {
        const RECT& primary = monitors.front().bounds;
        state.instructionOrigin = {primary.left - state.desktop.left + 24,
                                   primary.top - state.desktop.top + 24};
    }
    HWND previous = GetForegroundWindow();
    const bool disableOwner = owner && IsWindow(owner) && IsWindowEnabled(owner);
    HWND window = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kRegionClass,
        L"Clippy: select a region", WS_POPUP, state.desktop.left, state.desktop.top,
        width, height, owner, nullptr, instance, &state);
    if (!window) return false;
    if (disableOwner) EnableWindow(owner, FALSE);
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    SetFocus(window);
    UpdateWindow(window);
    MSG message{};
    while (!state.done) {
        const BOOL result = GetMessageW(&message, nullptr, 0, 0);
        if (result <= 0) {
            if (result == 0) PostQuitMessage(static_cast<int>(message.wParam));
            state.accepted = false;
            break;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    const bool accepted = state.accepted && HasRegion(state);
    const bool restoreFocus = GetForegroundWindow() == window;
    if (IsWindow(window)) DestroyWindow(window);
    if (disableOwner && IsWindow(owner)) EnableWindow(owner, TRUE);
    // Restore focus only while we still own it; an intentional Alt+Tab cancels the overlay.
    if (restoreFocus && previous && IsWindow(previous))
        SetForegroundWindow(previous);
    FlushDesktopComposition();
    if (accepted) {
        selected = {state.selection.left + state.desktop.left,
                    state.selection.top + state.desktop.top,
                    state.selection.right + state.desktop.left,
                    state.selection.bottom + state.desktop.top};
    }
    return accepted;
}

} // namespace clippy
