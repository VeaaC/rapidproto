#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Christian Vetter
#
# Quiesce this machine for benchmarking, and put it back afterwards.
#
#   tests/bench_box.sh setup     # apply, saving whatever was there before
#   tests/bench_box.sh restore   # put back exactly what setup found
#   tests/bench_box.sh status    # show current values, no changes
#
# These settings do NOT survive a reboot, which is the whole reason this exists: the bench otherwise
# produces confident-looking numbers on a box that cannot support them. `tests/bench.py` checks the
# same four settings on every run and points here.
#
# `restore` replays the SAVED values rather than assuming defaults -- guessing "the default" would
# silently change a machine that was deliberately configured. With no saved state it refuses.
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STATE="$REPO/build/bench_box.state"
FAILED=0   # writes that did not stick; setup/restore report a partial box rather than aborting
SKIPPED=0  # nodes that were absent or offline, so their saved value could not be replayed

SMT=/sys/devices/system/cpu/smt/control
TURBO=/sys/devices/system/cpu/intel_pstate/no_turbo
PARANOID=/proc/sys/kernel/perf_event_paranoid
# Only ONLINE cpus: an offline cpu (which is what `smt=off` leaves behind) keeps its cpufreq
# directory but the nodes are unreadable and unwritable, so including them fails the whole run.
# cpu0 has no `online` node and is always online.
governors() {
    local cpu online
    for cpu in /sys/devices/system/cpu/cpu[0-9]*; do
        online="$cpu/online"
        if [[ -e "$online" ]] && [[ "$(cat "$online" 2>/dev/null)" != "1" ]]; then continue; fi
        [[ -r "$cpu/cpufreq/scaling_governor" ]] && echo "$cpu/cpufreq/scaling_governor"
    done
    return 0  # never let the last loop test become this function's (and the script's) exit status
}

have() { [[ -e "$1" ]]; }
get()  { have "$1" && cat "$1" 2>/dev/null || echo "-"; }
# Writes need root; sudo -n first so a cached credential works without a prompt in a non-tty.
put() {
    local value=$1 path=$2
    have "$path" || { echo "  skip   $path (absent)"; return 0; }
    # Present but unreadable = an offline cpu's cpufreq node. `-r` tests permission bits only and
    # PASSES on those (mode 0644); the read itself fails EBUSY. Probe with an actual read.
    cat "$path" >/dev/null 2>&1 || {
        echo "  skip   $path (offline)"
        SKIPPED=$((SKIPPED + 1))
        return 0
    }
    if [[ "$(get "$path")" == "$value" ]]; then echo "  ok     $path = $value"; return 0; fi
    if ! sudo -n sh -c "echo $value > $path" 2>/dev/null && ! sudo sh -c "echo $value > $path"; then
        echo "  FAILED $path = $value (needs root)" >&2
        FAILED=$((FAILED + 1))  # accumulate: aborting here would leave the box half-configured
        return 0
    fi
    echo "  set    $path = $value"
}

status() {
    printf '  %-56s %s\n' "$SMT" "$(get $SMT)"
    printf '  %-56s %s\n' "$TURBO" "$(get $TURBO)   (1 = turbo disabled)"
    printf '  %-56s %s\n' "$PARANOID" "$(get $PARANOID)   (<=2 enables cyc/B, ins/B)"
    local first; first=$(governors | tr ' ' '\n' | head -1)
    printf '  %-56s %s\n' "scaling_governor (cpu0)" "$(get "$first")"
}

case "${1:-}" in
setup)
    mkdir -p "$(dirname "$STATE")"
    if [[ -f "$STATE" ]]; then
        echo "note: $STATE exists -- keeping the ORIGINAL saved values, not overwriting with"
        echo "      the current (already-quiesced) ones. Run 'restore' first to re-save."
    else
        { echo "smt=$(get $SMT)"
          echo "turbo=$(get $TURBO)"
          echo "paranoid=$(get $PARANOID)"
          for g in $(governors); do echo "gov:$g=$(get "$g")"; done
        } > "$STATE"
        echo "saved previous state -> $STATE"
    fi
    echo "applying benchmark settings:"
    put off "$SMT"
    put 1 "$TURBO"
    for g in $(governors); do put performance "$g"; done
    if have "$PARANOID"; then
        current=$(get $PARANOID)
        if [[ "$current" =~ ^-?[0-9]+$ ]] && ((current <= 2)); then
            echo "  ok     $PARANOID = $current"
        else
            sudo -n sysctl -w kernel.perf_event_paranoid=2 >/dev/null 2>&1 \
                || sudo sysctl -w kernel.perf_event_paranoid=2 >/dev/null
            echo "  set    $PARANOID = 2"
        fi
    fi
    if ((FAILED > 0)); then
        echo "$FAILED setting(s) did not apply -- the box is PARTIALLY configured." >&2
        echo "Run 'tests/bench_box.sh restore' to put it back, or re-run setup as root." >&2
        exit 1
    fi
    echo "done. Restore with: tests/bench_box.sh restore"
    ;;
restore)
    [[ -f "$STATE" ]] || { echo "no saved state at $STATE -- nothing to restore from." >&2; exit 1; }
    echo "restoring from $STATE:"
    while IFS='=' read -r key value; do
        [[ -n "${key:-}" && "$value" != "-" ]] || continue
        case "$key" in
        smt)      put "$value" "$SMT" ;;
        turbo)    put "$value" "$TURBO" ;;
        paranoid) sudo -n sysctl -w "kernel.perf_event_paranoid=$value" >/dev/null 2>&1 \
                      || sudo sysctl -w "kernel.perf_event_paranoid=$value" >/dev/null
                  echo "  set    $PARANOID = $value" ;;
        gov:*)    put "$value" "${key#gov:}" ;;
        esac
    done < "$STATE"
    if ((FAILED > 0 || SKIPPED > 0)); then
        echo "$((FAILED + SKIPPED)) setting(s) were not replayed; KEEPING $STATE so a re-run can" >&2
        echo "finish the job (deleting it would lose the original values for good)." >&2
        exit 1
    fi
    rm -f "$STATE"
    echo "done (saved state cleared)."
    ;;
status)
    status
    # Not `[[ -f ]] && echo` as the branch's last command: a false test would become the script's
    # exit status, so `if bench_box.sh status` would report failure on a clean state dir.
    if [[ -f "$STATE" ]]; then
        echo "  (saved pre-benchmark state present: $STATE)"
    fi
    ;;
*)
    sed -n '5,9p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    echo
    echo "current:"
    status
    exit 1
    ;;
esac
