#!/system/bin/sh
# Sourced by the installer. Do not enable set -e or set -u here.
ui_print "- ClipSync Module permissions"
set_perm "$MODPATH/system/bin/clipsyncd" 0 0 0755 u:object_r:system_file:s0 || abort "! clipsyncd permissions"
set_perm "$MODPATH/system/framework/clip.dex" 0 0 0444 u:object_r:system_file:s0 || abort "! clip.dex permissions"
