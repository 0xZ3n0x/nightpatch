#!/usr/bin/env bash
#
# verify.sh - run the framework-split verification plan.
#
# Stages (in order, each one skippable via flags):
#   static   build + dependency/symbol/boundary checks (no root)
#   load     insmod denial without engine, ordered load, device up (root)
#   inuse    rmmod trapkit must be refused while nightpatch is loaded (root)
#   game     enable the watcher and wait for the game to appear (root + game)
#   stress   insmod/enable/rmmod loop while the game runs; leak + oops scan (root)
#   session  wait for the game to exit and show the per-site summary (root + game)
#   unload   disable and remove both modules in the safe order (root)
#
# Usage:
#   ./scripts/verify.sh              # everything
#   ./scripts/verify.sh static load  # only these stages
#
# Environment knobs: FPS (default 165), LOOPS (default 20), WAIT_ARMED (default 120),
# WAIT_SESSION (default 900). Needs root via sudo (prompts normally).
#
set -u

TKMODULE=third_party/trapkit/build/kmod/trapkit.ko
KDIR=build/kmod
NIGHTCTL=build/nightctl
FPS=${FPS:-165}
LOOPS=${LOOPS:-20}
WAIT_ARMED=${WAIT_ARMED:-120}
WAIT_SESSION=${WAIT_SESSION:-900}
DMESG="sudo dmesg"
SUDO="sudo"

if [ $# -gt 0 ]; then stages="$*"; else stages="static load inuse game stress session unload"; fi

results=()
failures=0

say()  { printf '\n\033[1m=== %s ===\033[0m\n' "$1"; }
ok()   { results+=("PASS  $1"); printf '  \033[32mPASS\033[0m %s\n' "$1"; }
skip() { results+=("SKIP  $1"); printf '  \033[33mSKIP\033[0m %s\n' "$1"; }
bad()  { results+=("FAIL  $1"); printf '  \033[31mFAIL\033[0m %s\n' "$1"; failures=$((failures + 1)); }

has_stage() { echo " $stages " | grep -q " $1 "; }
loaded()    { lsmod | awk '{print $1}' | grep -q "^$1$"; }
device_up() { [ -e /dev/nightpatch ]; }

drop_app() { loaded nightpatch && $SUDO rmmod nightpatch >/dev/null 2>&1; return 0; }
drop_engine() { loaded trapkit && $SUDO rmmod trapkit >/dev/null 2>&1; return 0; }

# ---------------------------------------------------------------- static ----
stage_static() {
    say "static: build + boundary checks"
    make clean >/dev/null && make >/dev/null 2>&1
    if [ $? -eq 0 ]; then ok "make: clean build"; else bad "make: build failed"; return 1; fi

    dep="$(modinfo -F depends $KDIR/nightpatch.ko)"
    [ "$dep" = trapkit ] && ok "modinfo depends = trapkit" || bad "modinfo depends got '$dep'"

    n="$(cut -d$'\t' -f2 third_party/trapkit/build/kmod/Module.symvers | grep -c '^tk_')"
    [ "$n" -ge 1 ] && ok "Module.symvers exports $n tk_ symbols" || bad "no tk_ symbols exported"

    if grep -rn '#include' third_party/trapkit/src --include='*.c' --include='*.h' | grep -q nightpatch; then
        bad "boundary broken: trapkit includes nightpatch headers"
    else
        ok "boundary clean: no trapkit -> nightpatch includes"
    fi
}

# ------------------------------------------------------------------ load ----
stage_load() {
    say "load: engine dependency and ordered loading"
    drop_app; drop_engine

    # Engine must come first: the app alone must NOT load.
    if $SUDO insmod $KDIR/nightpatch.ko >/dev/null 2>&1; then
        bad "nightpatch loaded without trapkit (should be refused)"
        drop_app
    else
        if $DMESG | tail -30 | grep -q 'Unknown symbol tk_'; then
            ok "nightpatch alone refused: Unknown symbol tk_*"
        else
            ok "nightpatch alone refused (dmesg line not visible; check manually)"
        fi
    fi

    $SUDO insmod $TKMODULE || { bad "insmod trapkit"; return 1; }
    $SUDO insmod $KDIR/nightpatch.ko || { bad "insmod nightpatch"; return 1; }
    ok "ordered load: trapkit -> nightpatch"

    if device_up && sudo $NIGHTCTL status >/dev/null 2>&1; then
        ok "/dev/nightpatch up, nightctl status works"
    else
        bad "device or status broken"
    fi
}

# ----------------------------------------------------------------- inuse ----
stage_inuse() {
    say "inuse: rmmod trapkit must be refused"
    if ! loaded trapkit; then skip "trapkit not loaded (load stage failed?)"; return; fi
    if $SUDO rmmod trapkit >/dev/null 2>&1; then
        bad "rmmod trapkit succeeded while nightpatch is loaded"
        drop_engine 2>/dev/null
    else
        ok "rmmod trapkit refused (module dependency refcount)"
    fi
}

# ------------------------------------------------------------------ game ----
stage_game() {
    say "game: enable the watcher and wait for the game"
    if ! device_up; then skip "device not up"; return; fi
    sudo $NIGHTCTL enable "$FPS" >/dev/null || { bad "enable ioctl failed"; return 1; }
    ok "enabled at $FPS fps"

    printf '  waiting for armed state (up to %ss)\n' "$WAIT_ARMED"
    elapsed=0
    while [ $elapsed -lt $WAIT_ARMED ]; do
        if sudo $NIGHTCTL status 2>/dev/null | grep -q 'state      : armed'; then
            ok "watcher armed"
            sudo $NIGHTCTL status
            return
        fi
        sleep 2; elapsed=$((elapsed + 2))
    done
    skip "not armed within ${WAIT_ARMED}s (game not running?); continuing"
}

# ---------------------------------------------------------------- stress ----
stage_stress() {
    say "stress: $LOOPS cycles of rmmod/insmod with the watcher enabled"
    if ! device_up; then skip "device not up"; return; fi
    if ! grep -q 'state      : armed' <(sudo $NIGHTCTL status 2>/dev/null); then
        skip "watcher not armed; run the game first (the loop still works, but the armed swing is the real test)"
        return
    fi

    base=$(grep VmallocUsed /proc/meminfo | awk '{print $2}')
    base_lines=$($DMESG | wc -l)

    for i in $(seq 1 $LOOPS); do
        $SUDO rmmod nightpatch >/dev/null 2>&1
        $SUDO insmod $KDIR/nightpatch.ko >/dev/null 2>&1 || { bad "cycle $i: insmod failed"; break; }
        sudo $NIGHTCTL enable "$FPS" >/dev/null 2>&1
        sleep 1
        printf '  cycle %d/%d\r' "$i" "$LOOPS"
    done
    echo

    after=$(grep VmallocUsed /proc/meminfo | awk '{print $2}')
    delta=$((after - base))
    if [ $((delta < 0 ? -delta : delta)) -lt 5120 ]; then
        ok "VmallocUsed stable (delta ${delta} kB)"
    else
        bad "VmallocUsed moved by ${delta} kB (possible leak)"
    fi

    newlog="$($DMESG | tail -n +$((base_lines + 1)))"
    if echo "$newlog" | grep -qiE 'oops|BUG: kernel|unable to handle|call trace'; then
        echo "$newlog" | grep -iE 'oops|BUG: kernel|unable to handle|call trace' | head -5
        bad "kernel complaints in dmesg"
    else
        ok "no oops/call-trace in dmesg"
    fi

    if grep -q 'state      : armed' <(sudo $NIGHTCTL status 2>/dev/null); then
        ok "watcher re-armed after the loop"
    else
        skip "not armed after the loop (game may have exited)"
    fi
}

# --------------------------------------------------------------- session ----
stage_session() {
    say "session: exit the game and capture the summary"
    if ! grep -q 'state      : armed' <(sudo $NIGHTCTL status 2>/dev/null); then
        skip "not armed; nothing to end"
        return
    fi
    printf '  close the game now; waiting up to %ss\n' "$WAIT_SESSION"
    base_lines=$($DMESG | wc -l)
    elapsed=0
    while [ $elapsed -lt $WAIT_SESSION ]; do
        hit="$($DMESG | tail -n +$((base_lines + 1)) | grep -m1 'pid .* exited')"
        [ -n "$hit" ] && break
        sleep 2; elapsed=$((elapsed + 2))
    done
    if [ -n "$hit" ]; then
        $DMESG | tail -n +$((base_lines + 1)) | grep -E 'exited|hits|faults' | head -6
        ok "session-end summary captured"
    else
        skip "game did not exit within ${WAIT_SESSION}s"
    fi
}

# --------------------------------------------------------------- unload ----
stage_unload() {
    say "unload: disable and remove both modules in the safe order"
    device_up && sudo $NIGHTCTL disable >/dev/null 2>&1 && ok "disable ioctl"
    drop_app && ok "rmmod nightpatch"
    drop_engine && ok "rmmod trapkit"
    if lsmod | grep -qE 'trapkit|nightpatch'; then
        bad "modules still loaded after unload"
    else
        ok "both modules gone"
    fi
}

# ----------------------------------------------------------------- main ----
for s in $stages; do
    case $s in
        static|load|inuse|game|stress|session|unload) ;;
        *) echo "unknown stage: $s (static load inuse game stress session unload)" >&2; exit 2 ;;
    esac
done

if [ "$(id -u)" -eq 0 ]; then SUDO=""; DMESG="dmesg"; fi

for s in $stages; do
    "stage_$s" || true
done

echo
say "report"
for line in "${results[@]}"; do printf '%s\n' "$line"; done | sort | uniq -c | sort -k2

if [ "$failures" -gt 0 ]; then
    printf '\n%d failure(s)\n' "$failures"
    exit 1
fi
printf '\nall executed stages passed\n'
