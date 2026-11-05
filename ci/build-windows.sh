#!/bin/sh
# Cross-compiles picocalc-sim.exe (the window), picocalc-sim-console.exe (the same for a command
# line) and picocalc-sim-mkcard.exe for Windows x86-64 with MinGW-w64, each a single file that needs
# nothing beyond what Windows itself ships.
#   ci/build-windows.sh SOURCE_DIR BUILD_DIR
set -eu
src=$1
build=$2

if ! command -v x86_64-w64-mingw32-gcc >/dev/null 2>&1; then
    sudo=
    [ "$(id -u)" = 0 ] || sudo=sudo
    $sudo apt-get update -q
    DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -q --no-install-recommends mingw-w64
fi

cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_TOOLCHAIN_FILE="$(cd "$src" && pwd)/cmake/mingw-w64-x86_64.cmake"
cmake --build "$build" -j"$(nproc)"

# A MinGW runtime DLL (libwinpthread-1.dll, libgcc_s_seh-1.dll) would have to ship beside the .exe.
system_dlls='advapi32|gdi32|imm32|iphlpapi|kernel32|msvcrt|ole32|oleaut32|setupapi|shell32|user32|version|winmm|ws2_32'
for exe in "$build/picocalc-sim.exe" "$build/picocalc-sim-console.exe" "$build/tools/mkcard/picocalc-sim-mkcard.exe"; do
    extra=$(x86_64-w64-mingw32-objdump -p "$exe" | sed -n 's/^[[:space:]]*DLL Name: //p' |
        grep -viE "^($system_dlls)\.dll$" || true)
    if [ -n "$extra" ]; then
        echo "$exe needs DLLs that Windows does not ship:"
        echo "$extra"
        exit 1
    fi
done

# The tests under Wine run the console programs, which Wine would run the same either way.
subsystem() {
    x86_64-w64-mingw32-objdump -p "$1" | sed -n 's/^Subsystem[[:space:]]*[0-9a-f]*[[:space:]]*//p'
}
for check in "picocalc-sim.exe:(Windows GUI)" "picocalc-sim-console.exe:(Windows CUI)" "tools/mkcard/picocalc-sim-mkcard.exe:(Windows CUI)"; do
    exe=${check%%:*}
    got=$(subsystem "$build/$exe")
    if [ "$got" != "${check#*:}" ]; then
        echo "$exe is built for the subsystem $got, not ${check#*:}"
        exit 1
    fi
done
