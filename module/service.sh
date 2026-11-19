#!/system/bin/sh
set -euo pipefail
while [ "$(getprop sys.boot_completed)" != "1" ]; do
    sleep 1
done
mkdir -p /data/adb/clipsync
/system/bin/clipsyncd daemon >/dev/null 2>&1 &
