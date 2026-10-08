#include "../src/clippy.h"
#include <cstdio>
#include <cstring>
int main(int argc, char** argv) {
    if (argc > 1 && std::string(argv[1]) == "--probe-chrome") {
        std::wstring report;
        bool passed = clippy::ProbeChrome(report);
        std::wprintf(L"%ls\n", report.c_str());
        return passed ? 0 : 1;
    }
    if (argc > 1 && std::string(argv[1]) == "--test-paste") {
        // Exercise the real delivery path with generated pixels, never the desktop.
        clippy::Screenshot test;
        test.width = 160; test.height = 96;
        const int stride = (test.width * 3 + 3) & ~3;
        test.dib.resize(sizeof(BITMAPINFOHEADER) + stride * test.height);
        BITMAPINFOHEADER header{}; header.biSize = sizeof(header);
        header.biWidth = test.width; header.biHeight = test.height;
        header.biPlanes = 1; header.biBitCount = 24; header.biCompression = BI_RGB;
        header.biSizeImage = stride * test.height;
        std::memcpy(test.dib.data(), &header, sizeof(header));
        for (int y = 0; y < test.height; ++y) for (int x = 0; x < test.width; ++x) {
            auto* pixel = test.dib.data() + sizeof(header) + y * stride + x * 3;
            bool stripe = x > 20 && x < 140 && y > 32 && y < 64;
            pixel[0] = stripe ? 186 : 39; pixel[1] = stripe ? 221 : 27; pixel[2] = stripe ? 72 : 19;
        }
        HWND previous = GetForegroundWindow();
        HWND owner = CreateWindowExW(0, L"STATIC", L"Clippy generated test clipboard", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        std::wstring error;
        if (!owner || !clippy::SetScreenshotClipboard(owner, test, error)) {
            std::wprintf(L"Test clipboard failed: %ls\n", error.c_str());
            if (owner) DestroyWindow(owner); return 1;
        }
        auto settings = clippy::LoadSettings();
        auto result = clippy::DeliverToChatGPT(settings, false, previous, [](const std::wstring& message) {
            std::wprintf(L"%ls\n", message.c_str()); std::fflush(stdout);
        });
        DestroyWindow(owner);
        std::wprintf(L"Success: %d; sent: %d\n%ls\n", result.success, result.sent, result.message.c_str());
        return result.success && !result.sent ? 0 : 1;
    }
    return clippy::RunSelfTests();
}
