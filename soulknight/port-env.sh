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
SK_SWAP_LOOP=
echo "[sk-env] port-env v3 uid=$(id -u 2>/dev/null) esudo=${ESUDO:-none}"
sk_mem_kb() { awk -v k="$1:" '$1 == k { print $2; exit }' /proc/meminfo 2>/dev/null; }
sk_fstype() {
  # Filesystem type of the mount holding $1 (longest mount-point prefix wins).
  awk -v p="$1" '
    { mp = $2; gsub(/\040/, " ", mp)
      if ((p == mp || index(p, mp == "/" ? "/" : mp "/") == 1) && length(mp) > best) {
        best = length(mp); t = $3 } }
    END { print t }' /proc/mounts 2>/dev/null
}
# mkswap/swapon/losetup live in /sbin, which the port user's PATH often lacks.
sk_tool() {
  local d
  for d in /sbin /usr/sbin /bin /usr/bin; do
    [ -x "$d/$1" ] && { echo "$d/$1"; return 0; }
  done
  command -v "$1" 2>/dev/null
}
SK_MKSWAP=$(sk_tool mkswap); SK_SWAPON=$(sk_tool swapon)
SK_SWAPOFF=$(sk_tool swapoff); SK_LOSETUP=$(sk_tool losetup)
# Runs a step; on failure logs its last error line and returns non-zero.
sk_try() {
  local what=$1 out
  shift
  out=$("$@" 2>&1 >/dev/null) && return 0
  echo "[sk-mem] $what failed: $(printf '%s' "$out" | tail -n 1)"
  return 1
}
# Writes the swap image once and keeps it for later launches.
sk_make_swapfile() {
  local file=$1 size_mb=$2
  [ -f "$file" ] && return 0
  echo "[sk-mem] creating ${size_mb}MB swap image $file (first launch only, ~1 min)"
  sk_try dd $ESUDO dd if=/dev/zero of="$file" bs=1M count="$size_mb" \
    && sk_try chmod $ESUDO chmod 600 "$file" \
    && sk_try mkswap $ESUDO "$SK_MKSWAP" "$file" \
    && return 0
  $ESUDO rm -f "$file"
  return 1
}
sk_enable_swap() {
  local mem swap need dir fs free_kb size_mb file loop
  mem=$(sk_mem_kb MemTotal); swap=$(sk_mem_kb SwapTotal)
  case "$mem$swap" in ''|*[!0-9]*) return 0 ;; esac
  need=1572864   # want >= 1.5GB RAM+swap
  if [ $((mem + swap)) -ge "$need" ]; then
    echo "[sk-mem] RAM ${mem}kB + swap ${swap}kB: enough"
    return 0
  fi
  echo "[sk-mem] RAM ${mem}kB + swap ${swap}kB is low; adding swap"
  if [ -z "$SK_MKSWAP" ] || [ -z "$SK_SWAPON" ]; then
    echo "[sk-mem] mkswap/swapon not found on this CFW"
    return 0
  fi

  # 1) zram: compressed RAM, no SD access at all.
  [ -e /sys/block/zram0 ] || $ESUDO "$(sk_tool modprobe || echo modprobe)" zram num_devices=1 >/dev/null 2>&1
  if [ ! -e /sys/block/zram0/disksize ]; then
    echo "[sk-mem] zram: not in this kernel"
  elif [ "$(cat /sys/block/zram0/disksize 2>/dev/null)" != 0 ]; then
    echo "[sk-mem] zram0 already in use"
  else
    echo lz4 | $ESUDO tee /sys/block/zram0/comp_algorithm >/dev/null 2>&1
    if echo $((mem * 3 / 4))K | $ESUDO tee /sys/block/zram0/disksize >/dev/null 2>&1 \
       && sk_try "mkswap zram" $ESUDO "$SK_MKSWAP" /dev/zram0 \
       && sk_try "swapon zram" $ESUDO "$SK_SWAPON" -p 100 /dev/zram0; then
      SK_SWAP_ZRAM=/dev/zram0
      echo 100 | $ESUDO tee /proc/sys/vm/swappiness >/dev/null 2>&1
      echo "[sk-mem] zram swap on /dev/zram0 ($((mem * 3 / 4 / 1024))MB)"
      return 0
    fi
    echo 1 | $ESUDO tee /sys/block/zram0/reset >/dev/null 2>&1
  fi

  # 2) swapfile on a Linux filesystem (exFAT cannot hold one directly).
  for dir in "${HOME:-}" /home/ark /opt /; do
    [ -n "$dir" ] && [ -d "$dir" ] || continue
    fs=$(sk_fstype "$dir")
    case "$fs" in ext2|ext3|ext4|f2fs|xfs) ;; *) echo "[sk-mem] $dir: $fs, skipped"; continue ;; esac
    file="${dir%/}/.soulknight.swap"
    if [ ! -f "$file" ]; then
      free_kb=$(df -Pk "$dir" 2>/dev/null | awk 'NR == 2 { print $4 }')
      case "$free_kb" in ''|*[!0-9]*) free_kb=0 ;; esac
      size_mb=1024
      [ "$free_kb" -lt $(((size_mb + 256) * 1024)) ] && size_mb=512
      if [ "$free_kb" -lt $(((size_mb + 256) * 1024)) ]; then
        echo "[sk-mem] $dir: only $((free_kb / 1024))MB free, skipped"
        continue
      fi
      sk_make_swapfile "$file" "$size_mb" || continue
    fi
    if sk_try "swapon $file" $ESUDO "$SK_SWAPON" "$file"; then
      SK_SWAP_FILE=$file
      echo "[sk-mem] swapfile on $file"
      return 0
    fi
  done

  # 3) swap image on the exFAT game card, attached through a loop device.
  file="$GAMEDIR/home/.soulknight.swap"
  if [ -n "$SK_LOSETUP" ]; then
    free_kb=$(df -Pk "$GAMEDIR" 2>/dev/null | awk 'NR == 2 { print $4 }')
    case "$free_kb" in ''|*[!0-9]*) free_kb=0 ;; esac
    if [ -f "$file" ] || [ "$free_kb" -ge $((1280 * 1024)) ]; then
      if sk_make_swapfile "$file" 1024; then
        loop=$($ESUDO "$SK_LOSETUP" -f --show "$file" 2>&1)
        case "$loop" in
          /dev/loop*)
            if sk_try "swapon $loop" $ESUDO "$SK_SWAPON" "$loop"; then
              SK_SWAP_LOOP=$loop
              echo "[sk-mem] swap on $loop ($file)"
              return 0
            fi
            $ESUDO "$SK_LOSETUP" -d "$loop" 2>/dev/null ;;
          *) echo "[sk-mem] losetup failed: $loop" ;;
        esac
      fi
    else
      echo "[sk-mem] $GAMEDIR: only $((free_kb / 1024))MB free for a swap image"
    fi
  fi
  echo "[sk-mem] WARNING: could not add swap; expect stutter and OOM kills"
}
# Every 15 s: free RAM, free swap and major page faults (pages re-read from the
# SD card). Hundreds of faults per interval = memory thrash = the stutter.
SK_MON_PID=
sk_mem_monitor() {
  local prev cur
  prev=$(awk '$1 == "pgmajfault" { print $2 }' /proc/vmstat)
  while sleep 15; do
    cur=$(awk '$1 == "pgmajfault" { print $2 }' /proc/vmstat)
    echo "[sk-mem] avail=$(($(sk_mem_kb MemAvailable) / 1024))MB swapfree=$(($(sk_mem_kb SwapFree) / 1024))MB majfault/15s=$((cur - prev))"
    prev=$cur
  done
}
sk_port_cleanup() {
  [ -n "$SK_MON_PID" ] && { kill "$SK_MON_PID" 2>/dev/null; SK_MON_PID=; }
  if [ "${NXBOOTSTRAP_STATUS_FOR_CLEANUP:-}" = 137 ] && command -v dmesg >/dev/null 2>&1; then
    $ESUDO dmesg 2>/dev/null | grep -iE 'out of memory|oom-kill|killed process' | tail -n 5 \
      | sed 's/^/[sk-mem] dmesg: /'
  fi
  [ -n "$SK_SWAP_ZRAM" ] && { $ESUDO "$SK_SWAPOFF" "$SK_SWAP_ZRAM" 2>/dev/null
    echo 1 | $ESUDO tee /sys/block/zram0/reset >/dev/null 2>&1; SK_SWAP_ZRAM=; }
  [ -n "$SK_SWAP_FILE" ] && { $ESUDO "$SK_SWAPOFF" "$SK_SWAP_FILE" 2>/dev/null; SK_SWAP_FILE=; }
  [ -n "$SK_SWAP_LOOP" ] && { $ESUDO "$SK_SWAPOFF" "$SK_SWAP_LOOP" 2>/dev/null
    $ESUDO "$SK_LOSETUP" -d "$SK_SWAP_LOOP" 2>/dev/null; SK_SWAP_LOOP=; }
  sk_restore_governors
  sk_shader_cache_report
  return 0
}
# Start with as much free RAM as possible: drop clean page cache left by the
# extractor/splash.
sync
echo 3 | $ESUDO tee /proc/sys/vm/drop_caches >/dev/null 2>&1
echo "[sk-mem] avail at start: $(($(sk_mem_kb MemAvailable) / 1024))MB; biggest processes:"
ps -eo rss=,comm= --sort=-rss 2>/dev/null | head -n 5 | awk '{ printf "[sk-mem]   %5d MB %s
", $1 / 1024, $2 }'
sk_enable_swap

# First-use hitches (HeroRoom, map change, first skill cast) are Unity linking
# GL programs and uploading textures while the RK3326's CPU/GPU/DDR governors
# are still ramping up from idle. Pin them to "performance" for the session;
# sk_port_cleanup puts the original governors back.
SK_GOV_SAVED=
sk_perf_governors() {
  local f cur
  for f in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor \
           /sys/class/devfreq/*/governor; do
    [ -w "$f" ] || [ -n "$ESUDO" ] || continue
    [ -f "$f" ] || continue
    case "$f" in /sys/class/devfreq/*)
      # GPU and DDR controller only (other devfreq nodes may not offer it).
      case "$f" in *gpu*|*dmc*) ;; *) continue ;; esac ;;
    esac
    grep -qw performance "${f%governor}available_governors" 2>/dev/null || continue
    cur=$(cat "$f" 2>/dev/null)
    [ -n "$cur" ] && [ "$cur" != performance ] || continue
    if echo performance | $ESUDO tee "$f" >/dev/null 2>&1; then
      SK_GOV_SAVED="$SK_GOV_SAVED $f=$cur"
    fi
  done
  if [ -n "$SK_GOV_SAVED" ]; then
    echo "[sk-perf] governors -> performance:$SK_GOV_SAVED"
  else
    echo "[sk-perf] governors unchanged:$(for f in /sys/devices/system/cpu/cpufreq/policy*/scaling_governor /sys/class/devfreq/*/governor; do [ -f "$f" ] && printf ' %s=%s' "$f" "$(cat "$f" 2>/dev/null)"; done)"
  fi
}
sk_restore_governors() {
  local pair
  for pair in $SK_GOV_SAVED; do
    echo "${pair#*=}" | $ESUDO tee "${pair%%=*}" >/dev/null 2>&1
  done
  SK_GOV_SAVED=
}

# Unity keeps linked GL programs in <cacheDir>/UnityShaderCache/ so the hitch
# is paid once, not every launch. If the loader hands out an empty cacheDir the
# folder lands at "/" (not writable for the port user) and nothing is cached:
# point that path into home/ as the old "/" saves were.
mkdir -p "$GAMEDIR/home/UnityShaderCache"
if [ ! -e /UnityShaderCache ] && [ ! -L /UnityShaderCache ]; then
  $ESUDO ln -s "$GAMEDIR/home/UnityShaderCache" /UnityShaderCache 2>/dev/null
fi
sk_shader_cache_report() {
  local d n
  for d in "$GAMEDIR/home/UnityShaderCache" "$GAMEDIR/UnityShaderCache"; do
    [ -d "$d" ] || continue
    n=$(ls -A "$d" 2>/dev/null | wc -l)
    echo "[sk-perf] shader cache $d: $n file(s)"
  done
}
sk_shader_cache_report
sk_perf_governors
sk_mem_monitor 9>&- &
SK_MON_PID=$!
