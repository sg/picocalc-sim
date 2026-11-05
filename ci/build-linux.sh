#!/bin/sh
# Release build of picocalc-sim and picocalc-sim-mkcard that needs only glibc on the desktop it is
# copied to.
#   ci/build-linux.sh SOURCE_DIR BUILD_DIR
set -eu
src=$1
build=$2

# SDL compiles in only the backends whose headers it finds and dlopens their libraries at run
# time, so without these it silently builds a binary that cannot open a window.
pkgs="libx11-dev libxext-dev libxrandr-dev libxcursor-dev libxfixes-dev libxi-dev libxss-dev
      libxtst-dev libwayland-dev libxkbcommon-dev libegl-dev libdecor-0-dev wayland-protocols
      libpulse-dev libpipewire-0.3-dev libasound2-dev libdbus-1-dev"
# shellcheck disable=SC2086
if ! dpkg -s $pkgs >/dev/null 2>&1; then
    sudo=
    [ "$(id -u)" = 0 ] || sudo=sudo
    $sudo apt-get update -q
    DEBIAN_FRONTEND=noninteractive $sudo apt-get install -y -q --no-install-recommends $pkgs
fi

cmake -S "$src" -B "$build" -DCMAKE_BUILD_TYPE=Release
cmake --build "$build" -j"$(nproc)"

# an installed libsdl3-dev would be picked over the pinned SDL and become a runtime dependency
extra=$(ldd "$build/picocalc-sim" | grep -vE 'linux-vdso|libc\.so|libm\.so|ld-linux' || true)
if [ -n "$extra" ]; then
    echo "picocalc-sim depends on more than the C library:"
    echo "$extra"
    exit 1
fi

config=$(find "$build/_deps/sdl3-build" -path '*/build_config/SDL_build_config.h' | head -1)
for driver in VIDEO_DRIVER_X11 VIDEO_DRIVER_WAYLAND AUDIO_DRIVER_PULSEAUDIO \
              AUDIO_DRIVER_PIPEWIRE AUDIO_DRIVER_ALSA; do
    if ! grep -q "^#define SDL_$driver 1" "$config"; then
        echo "SDL was built without SDL_$driver"
        exit 1
    fi
done

echo "glibc symbols needed: $(objdump -T "$build/picocalc-sim" | grep -oE 'GLIBC_[0-9.]+' | sort -Vu | tail -1)"
