#!/bin/sh
# Boots each UF2 headless and fails if a core faults, the ROM is asked for something it lacks,
# the firmware never draws on the panel, or its sound is not recorded.
#   ci/boot-test.sh PCSIM UF2...
set -u
pcsim=$1
shift
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT
printf 'wait 4000\nscreenshot %s/panel.png\nquit 0\n' "$work" >"$work/boot.pcs"

failed=0
for uf2; do
    name=$(basename "$uf2")
    rm -f "$work/panel.png" "$work/sound.wav"
    # unthrottled because script waits are host time and a slow runner would otherwise see less boot
    timeout 120 "$pcsim" --headless --unthrottled --trace --flash none --no-card \
        --wav "$work/sound.wav" --script "$work/boot.pcs" "$uf2" >"$work/out.log" 2>"$work/err.log"
    rc=$?
    problem=
    if [ $rc -ne 0 ]; then
        problem="exited with $rc"
    elif grep -qE 'locked up|bus error|undefined instruction|unimplemented|unknown code|BKPT|ARM state|coprocessor instruction|divide by zero|SVC at' "$work/err.log"; then
        problem="faulted"
    elif ! lit=$(python3 - "$work/panel.png" <<'EOF'
import collections, struct, sys, zlib
data = open(sys.argv[1], "rb").read()
pos, idat, width = 8, b"", 0
while pos < len(data):
    n, kind = struct.unpack(">I4s", data[pos:pos + 8])
    if kind == b"IHDR":
        width = struct.unpack(">I", data[pos + 8:pos + 12])[0]
    elif kind == b"IDAT":
        idat += data[pos + 8:pos + 8 + n]
    pos += 12 + n
raw = zlib.decompress(idat)
stride = 1 + width * 3
pixels = collections.Counter(raw[r + 1 + x * 3:r + 4 + x * 3]
                             for r in range(0, len(raw), stride) for x in range(width))
lit = sum(pixels.values()) - pixels.most_common(1)[0][1]
print(lit)
sys.exit(0 if lit >= 100 else 1)
EOF
    ); then
        problem="panel is blank (${lit:-no screenshot})"
    elif ! peak=$(python3 - "$work/sound.wav" <<'EOF'
import struct, sys
d = open(sys.argv[1], "rb").read()
frames = struct.unpack("<I", d[40:44])[0] // 4
if d[:4] != b"RIFF" or d[8:16] != b"WAVEfmt " or frames < 48000 or len(d) != 44 + 4 * frames:
    sys.exit(1)
print(max(abs(v) for v in struct.unpack("<%dh" % (2 * frames), d[44:])))
EOF
    ); then
        problem="no sound recorded"
    fi

    if [ -n "$problem" ]; then
        echo "FAIL $name: $problem"
        sed 's/^/    /' "$work/err.log"
        failed=1
    else
        echo "ok   $name ($lit pixels drawn, sound peak $peak)"
    fi
done
exit $failed
