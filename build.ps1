# Builds the sprigNES firmware and the flasher, fully from the vendored
# toolchain in tools\ — nothing needs to be installed system-wide.
#
# Output:
#   build\sprignes.uf2         firmware only (ROM is added by the flasher)
#   build\SprigNESFlasher.exe  end-user GUI flasher, firmware embedded
#
# Note: native tools (cmake) print status to stderr; exit codes are checked
# manually, so ErrorActionPreference stays 'Continue'.
$root = $PSScriptRoot

$env:PATH = "$root\tools\cmake-4.3.3-windows-x86_64\bin;$root\tools\ninja;$root\tools\xpack-arm-none-eabi-gcc-15.2.1-1.1\bin;$env:PATH"
$env:PICO_SDK_PATH = "$root\tools\pico-sdk"
$env:PICO_EXTRAS_PATH = "$root\tools\pico-extras"
$env:PICO_TOOLCHAIN_PATH = "$root\tools\xpack-arm-none-eabi-gcc-15.2.1-1.1"

cmake -S $root -B "$root\build" -G Ninja -DCMAKE_BUILD_TYPE=Release "-DPython3_EXECUTABLE=$root\tools\python-embed\python.exe" "-Dpicotool_DIR=$root\tools\picotool-official\picotool"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
ninja -C "$root\build"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
Write-Output "Firmware ready: $root\build\sprignes.uf2"

# Build the flasher (embeds the firmware just built).
& "$root\build-flasher.ps1"
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

Write-Output "`nAll built. Ship build\SprigNESFlasher.exe to users."
