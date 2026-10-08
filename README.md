# Clippy

Clippy is a lightweight Windows utility for sending screenshots to ChatGPT quickly. Press a global hotkey to capture a monitor, both monitors, or a selected region; Clippy adds the image to an existing ChatGPT draft in Chrome and restores your previous window layout. A second shortcut can add the screenshot and send the complete draft.

It is a native C++ Win32 app for Windows 10/11 x64. It has no Qt or Chrome extension dependency and does not require an API key.

## Download

Download `Clippy.exe` from the latest GitHub Release and run it on Windows 10 or 11 x64. Open Chrome and sign in to ChatGPT before using a capture shortcut. The repository contains the source code; the compiled executable is distributed as a release asset.

## Use

Open Chrome and sign in to ChatGPT, then launch Clippy. It starts **paused** with capture mode **3** selected.

| Shortcut | Action |
|---|---|
| `Ctrl+Alt+Space` | Arm or pause global capture shortcuts. |
| `K` | Capture and add a screenshot to the existing ChatGPT draft. |
| `Shift+K` | Capture, add the screenshot, and send the **whole accumulated draft**, including existing text and attachments. |
| `1` | Select monitor 1, the primary display. |
| `2` | Select monitor 2. |
| `3` | Select both monitors 1 and 2; this is the default on every launch. |
| `4` | Select region mode. Drag a rectangle on the next capture; subsequent captures reuse it. Press `4` again to choose a new region. |

Mode keys select the capture area; press `K` or `Shift+K` afterward. Press Esc or right-click to cancel region selection.

**Pause before typing:** while armed, Clippy intercepts `K`, `Shift+K`, and mode keys `1`–`4` globally. Action shortcuts are configurable, and number shortcuts can be disabled in Settings. `Ctrl+Alt+Space` remains the fixed arm/pause shortcut.

The floating buttons also work while keyboard capture is paused. Their key labels pulse when a capture starts. Click the Armed/Paused badge to toggle shortcuts, drag the title to move the panel, or click Settings. The **−** control minimizes the panel to the tray while capture shortcuts stay available; click the tray icon to restore it. **×** exits Clippy and cancels a pending capture. Hover over the title-bar controls for their labels. Right-click the tray icon for settings, mode selection, floating-panel visibility, or Exit. Launching Clippy again opens the running app's Settings window.

## Chrome and settings

Clippy uses Windows UI Automation and Chrome's Tab Search to find an existing ChatGPT tab, preferring recent matches within the searched Chrome profile. With multiple profiles, Chrome windows are searched in window order. If no suitable open tab is found, it opens ChatGPT in Chrome. This version recognizes **English Chrome and ChatGPT accessibility labels**.

The panel starts at **88% opacity** while keeping its existing colors. Settings uses the same dark navy and teal theme, with an opacity slider and percentage display for **60–100% opacity**. Moving the slider previews transparency; click **Save settings** to keep it. Closing Settings restores the saved opacity. The slider also supports arrow keys. Settings lets you change the action shortcuts, new-tab load delay, upload-readiness timeout, and window-restoration behavior. The default restores the prior layout. The optional Minimize Chrome setting leaves the source Chrome window visible when your original window and ChatGPT destination use the same Chrome window.

The latest screenshot remains on the clipboard and replaces its previous contents. Clippy does not save a screenshot history to disk. Settings are saved in `%LOCALAPPDATA%\Clippy\settings.ini`.

If attachment or submission cannot be confirmed, check the ChatGPT draft before trying again. Clippy reports the problem and does not retry paste or send automatically. Chrome accessibility changes can affect recognition; the load delay and upload timeout can be increased for slow connections.

## Build and check

From this folder in PowerShell:

```powershell
.\setup-toolchain.ps1
.\build.ps1
.\test.ps1
```

Setup downloads the pinned LLVM-MinGW x64 toolchain into `.tools` and verifies its SHA-256 checksum. The build writes `dist\Clippy.exe`. You can also pass a compatible compiler to `build.ps1 -Compiler <path-to-clang++.exe>`.

To inspect the accessibility controls in open Chrome windows without switching tabs, changing the clipboard, pasting, or sending:

```powershell
.\test.ps1 -ProbeChrome
```

CMake is an alternative with an x64 MSVC or MinGW-compatible LLVM toolchain. For an installed Visual Studio C++ toolchain:

```powershell
cmake -S . -B build -A x64
cmake --build build --config Release
```

Validation so far: 179 self-tests and a 64-capture resource check passed. A live paste-only check successfully added a generated test image to the current ChatGPT draft without sending. Automatic submission still needs a live trial.
