#!/bin/sh
# Installs Wine and writes BUILD_DIR/pcsim-wine and BUILD_DIR/pcsim-mkcard-wine, which run the
# Windows build's .exe files so the same test scripts can drive them as the Linux binaries.
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

for exe in pcsim tools/mkcard/pcsim-mkcard; do
    wrapper="$build/$(basename "$exe")-wine"
    printf '#!/bin/sh\nWINEDEBUG=-all exec wine "%s" "$@"\n' "$build/$exe.exe" >"$wrapper"
    chmod +x "$wrapper"
done
wine --version
