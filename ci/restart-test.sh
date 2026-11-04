#!/bin/sh
# Restarts each UF2 part-way through its boot and fails if the boot after the restart ends in a
# different state from a fresh launch's, which would mean something outlived the restart.
#   ci/restart-test.sh PCSIM UF2...
# PCSIM_ROM, if set, is the RP2040 boot ROM for the Pico and Pico W images.
set -u
pcsim=$1
shift

# --trace prints a digest each time the machine stops, so a restarted run prints two. A flash of
# "none" outlives a restart, so --erase-flash starts both runs from the same blank one.
digests() {
    timeout 120 env "$@" "$pcsim" --headless --unthrottled --trace --flash none --erase-flash --no-card \
        ${PCSIM_ROM:+--rom "$PCSIM_ROM"} "$uf2" 2>&1 | tr -d '\r' | sed -n 's/.*state digest //p' | tr '\n' ' ' |
        sed 's/ $//'
}

failed=0
for uf2; do
    name=$(basename "$uf2")
    fresh=$(digests PCSIM_STOP_AT_MS=2000)
    runs=$(digests PCSIM_RESTART_AT_MS=1000 PCSIM_STOP_AT_MS=2000)
    count=$(echo "$runs" | wc -w)
    after=${runs##* }
    if [ "$count" -ne 2 ]; then
        echo "FAIL $name: the machine stopped $count times, not twice"
        failed=1
    elif [ "$fresh" != "$after" ]; then
        echo "FAIL $name: ${fresh:-no digest} from a fresh launch, $after after a restart"
        failed=1
    else
        echo "ok   $name ($after)"
    fi
done
exit $failed
