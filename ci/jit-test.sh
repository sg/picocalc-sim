#!/bin/sh
# Runs tests/jit's check of instructions the JIT compiles natively, then again on the block handlers
# and stepping one instruction at a time, and fails unless every run prints JIT PASS over UART0.
#   ci/jit-test.sh PICOCALC_SIM UF2
set -u
picocalc_sim=$1
uf2=$2
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

status=0
for engine in native blocks step; do
    case $engine in
        native) set -- ;;
        blocks) set -- PICOCALC_SIM_NO_JIT=1 ;;
        step) set -- PICOCALC_SIM_NO_BLOCKS=1 ;;
    esac
    env "$@" PICOCALC_SIM_STOP_AT_MS=3000 timeout 120 "$picocalc_sim" --headless --unthrottled --console \
        --flash none --no-card --board pico2 "$uf2" >"$work/out.log" 2>"$work/err.log"
    rc=$?
    summary=$(grep '^JIT ' "$work/out.log")
    if [ $rc -ne 0 ] || [ "${summary#JIT PASS}" = "$summary" ]; then
        echo "FAIL $(basename "$uf2") ($engine): ${summary:-no result (exit $rc)}"
        sed 's/^/    /' "$work/out.log" "$work/err.log"
        status=1
    else
        echo "ok   $(basename "$uf2") ($engine: ${summary#JIT PASS: })"
    fi
done
exit $status
