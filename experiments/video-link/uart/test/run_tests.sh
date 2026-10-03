#!/usr/bin/env bash
# Host tests of the link-mode code: the UART protocol (link_test) and the
# frame grabber + sender + display decoder chain on simulated video
# (grab_sim, CPU cost model of the ESP32-C5). No hardware needed.
#
#   test/run_tests.sh [DIR]      (from experiments/video-link/uart)
#
# DIR (optional) holds hardware captures from tools/link_dump.py
# (link_*_raw.bin, replayed through the channel-scan probe) and frames from
# tools/link_frame.py (frame_*.pgm, used for the codec statistics).
set -u
cd "$(dirname "$0")/.."
OUT=build-host
mkdir -p "$OUT"
DATA=${1:-}
CC=${CC:-gcc}
fail=0

run() {                      # run NAME EXPECTED_EXIT COMMAND...
    local name=$1 want=$2
    shift 2
    local log="$OUT/$name.log"
    "$@" >"$log" 2>&1
    local got=$?
    if [ "$got" -eq "$want" ]; then
        printf 'PASS  %-24s %s\n' "$name" "$(grep -E '^(rate|summary|probe|link_test)' "$log" | tail -1 | cut -c1-110)"
    else
        printf 'FAIL  %-24s exit %d, expected %d (see %s)\n' "$name" "$got" "$want" "$log"
        fail=1
    fi
}

C5=components/video_link_uart
$CC -O2 -Wall -Wextra -Iprotocol test/link_test.c -lm -o "$OUT/link_test" || exit 1
$CC -O2 -Wall -Wextra -DGRAB_SIM_COST -Itest/stubs -I$C5 -Iprotocol -I../../../main \
    test/grab_sim.c $C5/grab.c $C5/link_tx.c \
    -lm -o "$OUT/grab_sim" || exit 1

frames=()
if [ -n "$DATA" ]; then
    for f in "$DATA"/frame_*.pgm; do
        [ -f "$f" ] || continue
        # skip empty (all-black) frames
        if [ "$(tail -c 2000 "$f" | tr -d '\000' | wc -c)" -gt 0 ]; then frames+=("$f"); fi
    done
fi
run link_test 0 "$OUT/link_test" "${frames[@]}"

S="$OUT/grab_sim"
run pal_clean            0 $S --frames 12 --noise 0.6 --out "$OUT/pal_clean"
run pal_noisy_offset     0 $S --frames 10 --noise 1.6 --cfo -2 --ppm -120 --out "$OUT/pal_noisy"
run pal_smoothing_off    0 $S --frames 8 --noise 1.0 --smooth 0 --out "$OUT/pal_s0"
run ntsc                 0 $S --frames 10 --std ntsc --noise 1.0 --cfo 3 --ppm 50 --out "$OUT/ntsc"
run pal_gap_300ms        0 $S --frames 6 --gap-ms 300 --out "$OUT/pal_gap300"
run pal_gap_1800ms       0 $S --frames 5 --gap-ms 1800 --out "$OUT/pal_gap1800"
run mode_fine            0 $S --frames 8 --mode fine --out "$OUT/fine"
run mode_raw             0 $S --frames 6 --mode raw --out "$OUT/raw"
run baud_5M              0 $S --frames 10 --baud 5e6 --out "$OUT/baud5"
run probe_video          0 $S --probe --noise 1.0
run probe_offset_-5.6MHz 0 $S --probe --noise 1.0 --cfo -5.6
run probe_no_signal      0 $S --probe --nosignal
run grab_no_signal       1 $S --frames 1 --nosignal --out "$OUT/nosig"

if [ -n "$DATA" ]; then
    for f in "$DATA"/link_*_raw.bin; do
        [ -f "$f" ] || continue
        n=$(basename "$f" _raw.bin)
        "$S" --replay "$f" --probe >"$OUT/replay_$n.log" 2>&1
        printf 'INFO  replay %-17s %s\n' "$n" "$(cut -c1-100 "$OUT/replay_$n.log")"
    done
fi

[ $fail -eq 0 ] && echo "all host tests passed" || echo "HOST TESTS FAILED"
exit $fail
