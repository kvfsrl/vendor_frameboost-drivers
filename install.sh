#!/bin/sh
# OPLUS frameboost DLKM loader for peridot (SM8635)
#   --test    insmod each module in dependency order (non destructive)
#   --persist copy .ko into /vendor_dlkm/lib/modules/<kver>/ + write modules.load
#
# NOTE: cpufreq_uag is NOT in the order - this kernel's sched-walt does not
# export the WALT internals it needs (cpu_util_freq_walt/fmax_cap/
# sched_ravg_window), so it cannot link.
set -e

KVER=$(uname -r)
[ "$(id -u)" = 0 ] || { echo "need root" >&2; exit 1; }

DIR=$(dirname "$0")
ORDER="sched-walt oplus_bsp_schedtune oplus_bsp_sched_assist oplus_bsp_eas_opt oplus_bsp_frame_boost oplus_bsp_qos_sched ua_cpu_ioctl oplus_hans oplus_bsp_zram_opt"

verify() {
  m="$1"
  [ -f "$m" ] || { echo "missing: $m" >&2; return 1; }
  v=$(strings "$m" | grep -m1 '^vermagic=' )
  want="vermagic=$KVER "
  case "$v" in
    "$want"*) echo "OK   $v  ($(basename "$m"))" ;;
    *) echo "FAIL $v != $want ($(basename "$m"))" >&2 ;;
  esac
}

load_mod() {
  b="$1"
  m="$DIR/$b.ko"
  [ -f "$m" ] || m="/vendor_dlkm/lib/modules/$KVER/$b.ko"
  if lsmod | grep -q "^${b} "; then
    echo "    $b already loaded"; return 0
  fi
  echo "    insmod $m"
  insmod "$m" 2>&1 || echo "    !! insmod $b failed (rc=$?)"
}

case "$1" in
  --test)
    echo "== kernel: $KVER $([ -f /sys/kernel/kmi ] || echo '')"
    for b in $ORDER; do verify "$DIR/$b.ko"; done
    echo "== loading (order matters: walt first)"
    for b in $ORDER; do load_mod "$b"; done
    echo "== dmesg tail:"
    dmesg | tail -20
    ;;
  --persist)
    DEST="/vendor_dlkm/lib/modules/$KVER"
    mkdir -p "$DEST"
    for b in $ORDER; do
      m="$DIR/$b.ko"
      if [ -f "$m" ]; then verify "$m" && cp "$m" "$DEST/$b.ko" && echo "  copied $b.ko"; fi
    done
    : > "$DEST/modules.load"
    for b in $ORDER; do
      [ -f "$DEST/$b.ko" ] && echo "$b.ko" >> "$DEST/modules.load"
    done
    cat "$DEST/modules.load"
    echo "== vendor_dlkm updated. Reboot to autoload."
    ;;
  *)
    echo "usage: $0 [--test|--persist]"
    exit 1
    ;;
esac