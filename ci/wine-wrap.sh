#!/bin/sh
# Installs Wine and writes BUILD_DIR/picocalc-sim-wine and BUILD_DIR/picocalc-sim-mkcard-wine, which
# run the Windows build's .exe files so the same test scripts can drive them as the Linux binaries.
#   ci/wine-wrap.sh BUILD_DIR
set -eu
build=$(cd "$1" && pwd)

if ! command -v wine >/dev/null 2>&1; then
    sudo=
    [ "$(id -u)" = 0 ] || sudo=sudo
    $sudo apt-get update -q
    DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -q --no-install-recommends wine wine64
fi

# Wine's own diagnostics would land in the logs the tests search for faults.
export WINEDEBUG=-all
# Creating the Wine prefix takes a while, so do it once here rather than inside a test's timeout.
wineboot -i >/dev/null 2>&1 || true

# picocalc-sim-wine runs picocalc-sim-console.exe, the build a command line on Windows uses.
for exe in picocalc-sim:picocalc-sim-console picocalc-sim-mkcard:tools/mkcard/picocalc-sim-mkcard; do
    wrapper="$build/${exe%%:*}-wine"
    printf '#!/bin/sh\nWINEDEBUG=-all exec wine "%s" "$@"\n' "$build/${exe#*:}.exe" >"$wrapper"
    chmod +x "$wrapper"
done
wine --version
