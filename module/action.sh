#!/system/bin/sh
set -euo pipefail
MODDIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
exec /system/bin/sh "$MODDIR/webui.sh" toggle
