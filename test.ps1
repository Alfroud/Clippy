param([switch]$ProbeChrome)
$ErrorActionPreference = 'Stop'
$compiler = "$PSScriptRoot\.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe"
if (!(Test-Path -LiteralPath $compiler)) { throw 'Run .\setup-toolchain.ps1 first.' }
& $compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -static -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 "$PSScriptRoot\tests\checks.cpp" "$PSScriptRoot\src\capture.cpp" "$PSScriptRoot\src\chrome.cpp" "$PSScriptRoot\src\settings.cpp" -o "$PSScriptRoot\.tools\Clippy-checks.exe" -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -luiautomationcore -ladvapi32
if ($LASTEXITCODE -ne 0) { throw 'Test runner build failed.' }
if ($ProbeChrome) { & "$PSScriptRoot\.tools\Clippy-checks.exe" --probe-chrome }
else { & "$PSScriptRoot\.tools\Clippy-checks.exe" }
if ($LASTEXITCODE -ne 0) { throw "Checks failed ($LASTEXITCODE)." }
