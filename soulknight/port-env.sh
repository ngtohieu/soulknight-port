# Sourced by "Soul Knight.sh" right before the loader starts.

# Unity stays on its loading screen until the activity reports focus; the
# Skate3-derived loader skips nativeFocusChanged(true) unless forced.
export SK3_FOCUS=1

# Skate3-only age-gate scanner: the classes do not exist in this game.
export SK3_AGE_AUTO=0

# Shim for Unity 2020.3 under this loader (source: src/sk_jni_alias.c):
# fills the 11 imports the loader lacks (socketpair, remainderf, ...) and
# aliases getPackageResourcePath() to getPackageCodePath().
[ -f "$GAMEDIR/libsk_jni_alias.so" ] && BIN_PRELOAD="$GAMEDIR/libsk_jni_alias.so"

# Diagnostics: every JNI method Unity looks up goes to log.txt.
export BC_JNILOG=1

# Diagnostics: Unity/Android log output goes to log.txt.
export BC_LOGCAT=1

# Diagnostics: module base addresses and unresolved imports go to log.txt.
export BC_VERBOSE=1

# A frame stuck this long faults the render thread: the stack lands in log.txt
# and the process exits back to the menu (Select+Start is polled by that same
# thread, so a hang otherwise needs a power cycle).
export BC_WATCHDOG=120
