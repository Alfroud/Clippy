param([string]$Compiler = "$PSScriptRoot\.tools\llvm-mingw-20261006-ucrt-x86_64\bin\clang++.exe")
$ErrorActionPreference = 'Stop'
if (!(Test-Path -LiteralPath $Compiler)) { throw 'C++ compiler not found. Run .\setup-toolchain.ps1 or pass -Compiler.' }
New-Item -ItemType Directory -Force -Path "$PSScriptRoot\dist" | Out-Null
$sources = @('main.cpp','capture.cpp','chrome.cpp','settings.cpp') | ForEach-Object { Join-Path "$PSScriptRoot\src" $_ }
& $Compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -static -municode -mwindows -DUNICODE -D_UNICODE -D_WIN32_WINNT=0x0A00 -DWINVER=0x0A00 @sources -o "$PSScriptRoot\dist\Clippy.exe" -luser32 -lgdi32 -lshell32 -lole32 -loleaut32 -luuid -luiautomationcore -ladvapi32 -lcomctl32 -ldwmapi -lshlwapi -luxtheme
if ($LASTEXITCODE -ne 0) { throw "Build failed ($LASTEXITCODE)." }
Write-Output "Built $PSScriptRoot\dist\Clippy.exe"
