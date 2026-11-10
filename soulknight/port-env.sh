# Sourced by "Soul Knight.sh" right before the loader starts.

# Unity stays on its loading screen until the activity reports focus; the
# Skate3-derived loader skips nativeFocusChanged(true) unless forced.
export SK3_FOCUS=1

# Skate3-only age-gate scanner: the classes do not exist in this game.
export SK3_AGE_AUTO=0

# Shim for Unity 2020.3 under this loader (source: src/sk_jni_alias.c):
# fills the 11 imports the loader lacks (socketpair, remainderf, ...), maps
# StreamingAssets paths ("/assets/...") into the game dir, guards JNI
# calls on the bare ids the loader hands out for unknown fields and turns
# on Unity's -disableOpenSL so FMOD uses the loader's AudioTrack output.
[ -f "$GAMEDIR/libsk_jni_alias.so" ] && BIN_PRELOAD="$GAMEDIR/libsk_jni_alias.so"

# Before shim v7 the game's save files (persistentDataPath came out "")
# were written to "/" of the handheld. Move them into home/, where the shim
# now sends them, unless home/ already has that file.
for sk_save in game.data item_data.data item_data_backups.bytes statistic.data \
    season_data.data season_data_backups.bytes setting.data task.data \
    battles.data sandbox_config.data sandbox_maps.data analytics_data.data \
    analytics_data_backups.bytes; do
  if [ -f "/$sk_save" ] && [ ! -L "/$sk_save" ] && [ ! -e "$GAMEDIR/home/$sk_save" ]; then
    mkdir -p "$GAMEDIR/home" && mv -f "/$sk_save" "$GAMEDIR/home/$sk_save"
  fi
done
unset sk_save

# Diagnostics: Unity/Android log output goes to log.txt.
export BC_LOGCAT=1

# Extra diagnostics (module bases, unresolved imports, every stat() and JNI
# lookup); they slow the game down, so enable only when debugging.
# export BC_VERBOSE=1
# export BC_JNILOG=1

# A frame stuck this long faults the render thread: the stack lands in log.txt
# and the process exits back to the menu (Select+Start is polled by that same
# thread, so a hang otherwise needs a power cycle).
export BC_WATCHDOG=120

# Low-RAM handhelds (RK3326 on dArkOS: ~895MB, no swap) get the game SIGKILLed
# by the kernel OOM killer while the first dungeon scene loads (status 137 right
# after HeroRoom). muOS ships swap/zram and is fine. When RAM+swap is short, add
# swap for this session: zram first (no SD wear), else a swapfile on a native
# Linux filesystem (exFAT /roms cannot hold one). Undone by sk_port_cleanup.
SK_SWAP_ZRAM=
SK_SWAP_FILE=
sk_mem_kb() { awk -v k="$1:" '$1 == k { print $2; exit }' /proc/meminfo 2>/dev/null; }
sk_fstype() {
  # Filesystem type of the mount holding $1 (longest mount-point prefix wins).
  awk -v p="$1" '
    { mp = $2; gsub(/\040/, " ", mp)
      if ((p == mp || index(p, mp == "/" ? "/" : mp "/") == 1) && length(mp) > best) {
        best = length(mp); t = $3 } }
    END { print t }' /proc/mounts 2>/dev/null
}
sk_enable_swap() {
  local mem swap need zdev dir fs free_kb size_mb
  mem=$(sk_mem_kb MemTotal); swap=$(sk_mem_kb SwapTotal)
  case "$mem$swap" in ''|*[!0-9]*) return 0 ;; esac
  need=1572864   # want >= 1.5GB RAM+swap
  [ $((mem + swap)) -ge "$need" ] && return 0
  echo "[sk-mem] RAM ${mem}kB + swap ${swap}kB is low; adding swap"

  # 1) zram
  [ -e /sys/block/zram0 ] || $ESUDO modprobe zram num_devices=1 2>/dev/null
  if [ -e /sys/block/zram0/disksize ] && [ "$(cat /sys/block/zram0/disksize 2>/dev/null)" = 0 ]; then
    zdev=/dev/zram0
    echo lz4 | $ESUDO tee /sys/block/zram0/comp_algorithm >/dev/null 2>&1
    if echo $((mem * 3 / 4))K | $ESUDO tee /sys/block/zram0/disksize >/dev/null 2>&1 \
       && $ESUDO mkswap "$zdev" >/dev/null 2>&1 \
       && $ESUDO swapon -p 100 "$zdev" 2>/dev/null; then
      SK_SWAP_ZRAM=$zdev
      echo "[sk-mem] zram swap on $zdev ($((mem * 3 / 4 / 1024))MB)"
      return 0
    fi
    echo 1 | $ESUDO tee /sys/block/zram0/reset >/dev/null 2>&1
  fi

  # 2) swapfile, kept between launches so it is only written once
  for dir in "$GAMEDIR" "${HOME:-}" /home/ark /opt /; do
    [ -n "$dir" ] && [ -d "$dir" ] || continue
    fs=$(sk_fstype "$dir")
    case "$fs" in ext2|ext3|ext4|f2fs|xfs) ;; *) continue ;; esac
    SK_SWAP_FILE="${dir%/}/.soulknight.swap"
    size_mb=1024
    if [ ! -f "$SK_SWAP_FILE" ]; then
      free_kb=$(df -Pk "$dir" 2>/dev/null | awk 'NR == 2 { print $4 }')
      case "$free_kb" in ''|*[!0-9]*) free_kb=0 ;; esac
      [ "$free_kb" -lt $(((size_mb + 256) * 1024)) ] && size_mb=512
      [ "$free_kb" -lt $(((size_mb + 256) * 1024)) ] && { SK_SWAP_FILE=; continue; }
      echo "[sk-mem] creating ${size_mb}MB swapfile $SK_SWAP_FILE (first launch only)"
      $ESUDO dd if=/dev/zero of="$SK_SWAP_FILE" bs=1M count="$size_mb" 2>/dev/null \
        && $ESUDO chmod 600 "$SK_SWAP_FILE" \
        && $ESUDO mkswap "$SK_SWAP_FILE" >/dev/null 2>&1 \
        || { $ESUDO rm -f "$SK_SWAP_FILE"; SK_SWAP_FILE=; continue; }
    fi
    if $ESUDO swapon "$SK_SWAP_FILE" 2>/dev/null; then
      echo "[sk-mem] swapfile on $SK_SWAP_FILE"
      return 0
    fi
    SK_SWAP_FILE=
  done
  echo "[sk-mem] WARNING: could not add swap; the game may be OOM-killed on load"
}
sk_port_cleanup() {
  if [ "${NXBOOTSTRAP_STATUS_FOR_CLEANUP:-}" = 137 ] && command -v dmesg >/dev/null 2>&1; then
    $ESUDO dmesg 2>/dev/null | grep -iE 'out of memory|oom-kill|killed process' | tail -n 5 \
      | sed 's/^/[sk-mem] dmesg: /'
  fi
  [ -n "$SK_SWAP_ZRAM" ] && { $ESUDO swapoff "$SK_SWAP_ZRAM" 2>/dev/null
    echo 1 | $ESUDO tee /sys/block/zram0/reset >/dev/null 2>&1; SK_SWAP_ZRAM=; }
  [ -n "$SK_SWAP_FILE" ] && { $ESUDO swapoff "$SK_SWAP_FILE" 2>/dev/null; SK_SWAP_FILE=; }
  return 0
}
sk_enable_swap
