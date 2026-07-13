# Builds SprigNESFlasher.exe: embeds the freshly-built firmware UF2 as a
# resource, then compiles the Win32 GUI with MinGW.
$root = $PSScriptRoot
$fw = "$root\build\sprignes.uf2"
$flasher = "$root\flasher"

if (-not (Test-Path $fw)) {
    Write-Error "Firmware not built yet. Run .\build.ps1 first."
    exit 1
}

$mingw = 'D:\c libs\mingw64\bin'
$gcc = "$mingw\gcc.exe"
$windres = "$mingw\windres.exe"

# Stage the firmware next to the .rc so the resource compiler finds it.
Copy-Item $fw "$flasher\sprignes_firmware.uf2" -Force

& $windres "$flasher\flasher.rc" "$flasher\flasher_res.o"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

& $gcc "$flasher\flasher.c" "$flasher\flasher_res.o" `
    -o "$root\build\SprigNESFlasher.exe" `
    -O2 -static -mwindows `
    -lcomdlg32 -lshell32
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Remove-Item "$flasher\flasher_res.o", "$flasher\sprignes_firmware.uf2" -ErrorAction SilentlyContinue
Write-Output "Built: $root\build\SprigNESFlasher.exe"
