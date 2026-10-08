$ErrorActionPreference = 'Stop'
$toolsDirectory = Join-Path $PSScriptRoot '.tools'
$archive = Join-Path $toolsDirectory 'llvm-mingw-20261006-ucrt-x86_64.zip'
New-Item -ItemType Directory -Force -Path $toolsDirectory | Out-Null
if (!(Test-Path -LiteralPath $archive)) {
    Invoke-WebRequest -Uri 'https://github.com/mstorsjo/llvm-mingw/releases/download/20261006/llvm-mingw-20261006-ucrt-x86_64.zip' -OutFile $archive
}
$digest = (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant()
if ($digest -ne '317492c456aa27ee607a5919f1d2d38dcdc1112516a24d0bf4b00d078f52d17a') { throw 'Toolchain checksum mismatch.' }
Expand-Archive -LiteralPath $archive -DestinationPath $toolsDirectory -Force
Write-Output 'Portable compiler ready. Run .\build.ps1'
