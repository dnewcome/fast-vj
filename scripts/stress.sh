#!/usr/bin/env bash
# stress.sh — run fast-vj under a stress patch for a fixed window and
# summarize FPS. Useful for comparing hardware and detecting frame-rate
# regressions or drift over long runs.
#
# Usage:   scripts/stress.sh [duration_seconds] [--no-vsync] [-p PATCH]
# Env vars: BINARY, PATCH, MEDIA_DIR, OSC_PORT, EXTRA_ARGS

set -euo pipefail

DURATION="${DURATION:-60}"
BINARY="${BINARY:-./build/fast-vj}"
PATCH="${PATCH:-patches/stress.lua}"
MEDIA_DIR="${MEDIA_DIR:-media}"
OSC_PORT="${OSC_PORT:-9000}"
EXTRA_ARGS="${EXTRA_ARGS:-}"
NO_VSYNC=0

while [ $# -gt 0 ]; do
    case "$1" in
        --no-vsync) NO_VSYNC=1; shift ;;
        -p)         PATCH="$2"; shift 2 ;;
        -h|--help)  sed -n '2,7p' "$0"; exit 0 ;;
        *)          DURATION="$1"; shift ;;
    esac
done

if [ "$NO_VSYNC" = "1" ]; then
    EXTRA_ARGS="$EXTRA_ARGS -V"
fi

if [ ! -x "$BINARY" ]; then
    echo "stress: $BINARY not found or not executable" >&2
    exit 1
fi
if [ ! -f "$PATCH" ]; then
    echo "stress: $PATCH not found" >&2
    exit 1
fi

echo "=== machine ==="
uname -a
if command -v glxinfo >/dev/null 2>&1; then
    glxinfo 2>/dev/null | grep -E "OpenGL (renderer|version)" || true
fi
if [ -r /proc/cpuinfo ]; then
    grep -m1 -E "model name|Hardware|Model" /proc/cpuinfo || true
fi
echo "vsync   : $([ "$NO_VSYNC" = "1" ] && echo OFF || echo ON)"
echo "patch   : $PATCH"
echo

LOG=$(mktemp -t fast-vj-stress.XXXXXX)
FPS_FILE="${LOG}.fps"
trap 'rm -f "$LOG" "$FPS_FILE"' EXIT

echo "=== run: ${DURATION}s ($BINARY $MEDIA_DIR $OSC_PORT -f -s $PATCH$EXTRA_ARGS) ==="
# shellcheck disable=SC2086
"$BINARY" "$MEDIA_DIR" "$OSC_PORT" -f -s "$PATCH" $EXTRA_ARGS > "$LOG" 2>&1 &
PID=$!

sleep "$DURATION"

kill -INT "$PID" 2>/dev/null || true
for _ in 1 2 3 4 5; do
    kill -0 "$PID" 2>/dev/null || break
    sleep 1
done
kill -KILL "$PID" 2>/dev/null || true
wait "$PID" 2>/dev/null || true

# Drop the first sample (warm-up) and keep timeline order for drift.
grep '^fps:' "$LOG" | awk 'NR>1 { print $2 }' > "$FPS_FILE"

if [ ! -s "$FPS_FILE" ]; then
    echo "stress: no fps samples captured. full log:" >&2
    cat "$LOG" >&2
    exit 1
fi

echo
echo "=== fps summary ==="
awk '
{
    a[NR] = $1
    sum  += $1
    if (NR == 1 || $1 > max) max = $1
    if (NR == 1 || $1 < min) min = $1
}
END {
    n = NR
    mean = sum / n
    q = int(n / 4); if (q < 1) q = 1
    first = 0; last = 0
    for (i = 1;       i <= q; i++) first += a[i]
    for (i = n-q+1;   i <= n; i++) last  += a[i]
    first /= q; last /= q
    printf "samples : %d (1 sample/sec)\n", n
    printf "min     : %.1f fps\n", min
    printf "mean    : %.1f fps\n", mean
    printf "max     : %.1f fps\n", max
    printf "first %d : %.1f fps\n", q, first
    printf "last %d  : %.1f fps\n", q, last
    printf "drift   : %+.1f fps  (last - first)\n", last - first
}' "$FPS_FILE"
