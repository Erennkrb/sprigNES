# Downloads the vendored build toolchain into tools\. These are not committed
# to git because of their size. Run this once after cloning, then .\build.ps1.
#
# Populates: ARM GCC, CMake, Ninja, embeddable Python, prebuilt picotool,
# pico-sdk (+ tinyusb submodule) and pico-extras.
$root = Split-Path $PSScriptRoot
$tools = "$root\tools"
$dl = "$tools\dl"
New-Item -ItemType Directory -Force $dl | Out-Null

function Get-Zip($url, $out) {
    if (-not (Test-Path $out)) {
        Write-Output "Downloading $url"
        Invoke-WebRequest -Uri $url -OutFile $out
    }
}

# --- portable binaries ---
$arm   = 'https://github.com/xpack-dev-tools/arm-none-eabi-gcc-xpack/releases/download/v15.2.1-1.1/xpack-arm-none-eabi-gcc-15.2.1-1.1-win32-x64.zip'
$cmake = 'https://github.com/Kitware/CMake/releases/download/v4.3.3/cmake-4.3.3-windows-x86_64.zip'
$ninja = 'https://github.com/ninja-build/ninja/releases/latest/download/ninja-win.zip'
$py    = 'https://www.python.org/ftp/python/3.12.8/python-3.12.8-embed-amd64.zip'
$pt    = 'https://github.com/raspberrypi/pico-sdk-tools/releases/download/v2.3.0-0/picotool-2.3.0-x64-win.zip'

Get-Zip $arm   "$dl\arm-gcc.zip"
Get-Zip $cmake "$dl\cmake.zip"
Get-Zip $ninja "$dl\ninja.zip"
Get-Zip $py    "$dl\python-embed.zip"
Get-Zip $pt    "$dl\picotool.zip"

Expand-Archive "$dl\arm-gcc.zip"      $tools -Force
Expand-Archive "$dl\cmake.zip"        $tools -Force
Expand-Archive "$dl\ninja.zip"        "$tools\ninja" -Force
Expand-Archive "$dl\python-embed.zip" "$tools\python-embed" -Force
Expand-Archive "$dl\picotool.zip"     "$tools\picotool-official" -Force

# --- Raspberry Pi SDKs ---
if (-not (Test-Path "$tools\pico-sdk")) {
    git clone --depth 1 --branch master https://github.com/raspberrypi/pico-sdk "$tools\pico-sdk"
    git -C "$tools\pico-sdk" submodule update --init --depth 1 lib/tinyusb
}
if (-not (Test-Path "$tools\pico-extras")) {
    git clone --depth 1 https://github.com/raspberrypi/pico-extras "$tools\pico-extras"
}

Write-Output "`nToolchain ready. Now run .\build.ps1"
