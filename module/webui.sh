#!/system/bin/sh
# Read module status and serialize clipboard sync controls for the WebUI and action button.
set -euo pipefail
umask 077
MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
WORKDIR=/data/adb/clipsync
PID=0
STATE=stopped

read_state() {
    PID=0
    STATE=stopped
    [ -f "$WORKDIR/clipsyncd.pid" ] || return 0
    PID=$(cat "$WORKDIR/clipsyncd.pid")
    case "$PID" in ''|*[!0-9]*) PID=0; return 0 ;; esac
    if [ "$PID" -le 1 ] || ! kill -0 "$PID" 2>/dev/null; then PID=0; return 0; fi
    if ! EXE=$(readlink "/proc/$PID/exe"); then PID=0; return 0; fi
    case "$EXE" in */clipsyncd|*/clipsyncd\ \(deleted\)) ;; *) PID=0; return 0 ;; esac
    STATE=unknown
    if [ -f "$WORKDIR/state" ]; then
        VALUE=$(cat "$WORKDIR/state")
        case "$VALUE" in running|paused) STATE=$VALUE ;; esac
    fi
}

print_status() {
    read_state
    printf 'state\t%s\npid\t%s\n' "$STATE" "$PID"
    printf 'version\t%s\n' "$(sed -n 's/^version=//p' "$MODDIR/module.prop" | tr -d '\r\n\t')"
    printf 'device\t%s\n' "$(getprop ro.product.model | tr -d '\r\n\t')"
    printf 'android\t%s\n' "$(getprop ro.build.version.release | tr -d '\r\n\t')"
    if [ -s "$WORKDIR/psk" ]; then printf 'psk\tconfigured\n'; else printf 'psk\tempty\n'; fi
    ip -o -4 addr show up | awk '$2 ~ /^(wlan|ap)/ { print "interface\t" $2 "\t" $4 }'
    if [ -f "$WORKDIR/peers.state" ]; then
        awk -F '\t' -v state="$STATE" 'BEGIN { OFS="\t" } NF == 6 && $1 == "peer" { if (state != "running" && state != "paused") $6="offline"; print }' "$WORKDIR/peers.state"
    fi
}

lock_control() {
    mkdir -p "$WORKDIR"
    BUSYBOX=/data/adb/ksu/bin/busybox
    if [ ! -x "$BUSYBOX" ]; then BUSYBOX=/data/adb/magisk/busybox; fi
    if [ ! -x "$BUSYBOX" ]; then echo 'Manager BusyBox is unavailable.' >&2; exit 1; fi
    exec "$BUSYBOX" flock -n "$WORKDIR/webui.lock" /system/bin/sh "$MODDIR/webui.sh" "locked-$1"
}

set_state() {
    read_state
    case "$STATE" in running|paused) ;; *) echo 'Sync daemon is not ready.' >&2; exit 1 ;; esac
    TARGET=$1
    if [ "$TARGET" = toggle ]; then
        if [ "$STATE" = running ]; then TARGET=paused; else TARGET=running; fi
    fi
    if [ "$STATE" != "$TARGET" ]; then
        CONTROL_PID=$PID
        kill -USR1 "$CONTROL_PID"
        ATTEMPTS=0
        while [ "$ATTEMPTS" -lt 20 ]; do
            sleep 0.1
            read_state
            if [ "$PID" != "$CONTROL_PID" ]; then echo 'Sync daemon exited during control.' >&2; exit 1; fi
            [ "$STATE" != "$TARGET" ] || break
            ATTEMPTS=$((ATTEMPTS + 1))
        done
        if [ "$STATE" != "$TARGET" ]; then echo 'Sync state change timed out.' >&2; exit 1; fi
    fi
    printf '%s\n' "$STATE"
}

case "${1:-status}" in
    status) print_status ;;
    scan) exec /system/bin/clipsyncd scan ;;
    add-peer) [ "$#" -eq 2 ] || exit 2; exec /system/bin/clipsyncd add-peer "$2" ;;
    pause|resume|toggle) lock_control "$1" ;;
    locked-pause) set_state paused ;;
    locked-resume) set_state running ;;
    locked-toggle) set_state toggle ;;
    *) echo 'usage: webui.sh [status|scan|add-peer IPv4|pause|resume|toggle]' >&2; exit 2 ;;
esac
