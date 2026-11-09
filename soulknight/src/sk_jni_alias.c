/*
 * LD_PRELOAD shim for the NextOS Unity loader running Soul Knight 4.3.8.
 *
 * 1. JNI alias. Unity 2020.3 may ask Context.getPackageResourcePath(); the
 *    loader (written for Unity 2022.3) only emulates getPackageCodePath(). The
 *    loader matches JNI method names with strcmp(), so this strcmp treats the
 *    two names as equal and the existing handler answers both.
 *
 * 2. Missing imports. The loader resolves Android imports only from its own
 *    bionic table; libunity/libil2cpp 2020.3 need 11 symbols it lacks
 *    (socketpair, remainderf, fork, ...). Those GOT slots stay NULL and the
 *    first call jumps to address 0 (socketpair from a "Background Job." thread).
 *    Before every pthread_create - Unity code only creates threads once the
 *    modules are relocated - the shim walks the loader's module list
 *    (bc_iterate_mods, exported by the loader) and fills those slots.
 *
 * 3. StreamingAssets. Unity's APK root is "" under this loader (data loads
 *    from the relative "assets/bin/Data"), so Application.streamingAssetsPath
 *    is "jar:file://!/assets" and Unity resolves its files to "/assets/...".
 *    The file calls the loader forwards to glibc are interposed and those
 *    paths are mapped back to the relative "assets/..." in the game dir.
 */
#define _GNU_SOURCE
#include <dirent.h>
#include <dlfcn.h>
#include <elf.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

/* ---------------------------------------------------------------- logging */

static void sk_log(const char *msg)
{
    size_t n = 0;
    while (msg[n])
        n++;
    if (write(2, msg, n) < 0) {
        /* nothing to do */
    }
}

static void sk_log_num(const char *prefix, unsigned long value, const char *suffix)
{
    char buf[160];
    snprintf(buf, sizeof(buf), "%s%lu%s", prefix, value, suffix);
    sk_log(buf);
}

/* ---------------------------------------------------------------- strcmp */

static const char kAsked[] = "getPackageResourcePath";
static const char kKnown[] = "getPackageCodePath";
static int g_alias_logged;

static int plain_strcmp(const char *a, const char *b)
{
    const unsigned char *x = (const unsigned char *)a;
    const unsigned char *y = (const unsigned char *)b;
    while (*x && *x == *y) {
        x++;
        y++;
    }
    return *x - *y;
}

static int is_alias_pair(const char *a, const char *b)
{
    return plain_strcmp(a, kAsked) == 0 && plain_strcmp(b, kKnown) == 0;
}

int strcmp(const char *a, const char *b)
{
    /* Fast path: the alias pair both start with "getPackage". */
    if (a[0] == 'g' && b[0] == 'g' && (is_alias_pair(a, b) || is_alias_pair(b, a))) {
        if (!g_alias_logged) {
            g_alias_logged = 1;
            sk_log("[sk-shim] getPackageResourcePath -> getPackageCodePath\n");
        }
        return 0;
    }
    return plain_strcmp(a, b);
}

/* ------------------------------------------------- bionic-ABI termios */

/* Bionic's struct termios is the kernel one; glibc's is larger, so these
 * go straight to the ioctls instead of glibc's tc* functions. */
static int sk_tcgetattr(int fd, void *t)
{
    return ioctl(fd, TCGETS, t);
}

static int sk_tcsetattr(int fd, int action, const void *t)
{
    if (action < 0 || action > 2)
        action = 0;
    return ioctl(fd, TCSETS + action, t);
}

static int sk_tcflush(int fd, int queue)
{
    return ioctl(fd, TCFLSH, queue);
}

/* ---------------------------------------------------- import fix-ups */

struct sk_import {
    const char *name;
    void *impl;
};

static const struct sk_import kImports[] = {
    { "socketpair", (void *)socketpair },
    { "remainderf", (void *)remainderf },
    { "execl", (void *)execl },
    { "kill", (void *)kill },
    { "perror", (void *)perror },
    { "waitpid", (void *)waitpid },
    { "fork", (void *)fork },
    { "environ", (void *)&environ },
    { "tcflush", (void *)sk_tcflush },
    { "tcgetattr", (void *)sk_tcgetattr },
    { "tcsetattr", (void *)sk_tcsetattr },
};

#define SK_MAX_SLOTS 64

struct sk_slot {
    uintptr_t *where;
    uintptr_t value;
};

/* Same layout the loader passes to bc_iterate_mods callbacks. */
struct sk_mod_info {
    uintptr_t addr;
    const char *name;
    const Elf64_Phdr *phdr;
    uint16_t phnum;
};

typedef int (*sk_iter_cb)(struct sk_mod_info *, size_t, void *);
typedef int (*sk_iterate_fn)(sk_iter_cb, void *);

static struct sk_slot g_slots[SK_MAX_SLOTS];
static int g_slot_count;
static int g_scanned_unity;
static int g_scanned_il2cpp;
static volatile int g_lock;

static const char *base_name(const char *path)
{
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

static uintptr_t dyn_ptr(uintptr_t base, uintptr_t value)
{
    return value >= base ? value : base + value;
}

static void scan_relocs(uintptr_t base, const Elf64_Rela *rela, size_t count,
                        const Elf64_Sym *symtab, const char *strtab)
{
    for (size_t i = 0; i < count; i++) {
        uint32_t type = ELF64_R_TYPE(rela[i].r_info);
        uint32_t sym = ELF64_R_SYM(rela[i].r_info);
        if (sym == 0 || (type != R_AARCH64_JUMP_SLOT && type != R_AARCH64_GLOB_DAT &&
                         type != R_AARCH64_ABS64))
            continue;
        const char *name = strtab + symtab[sym].st_name;
        for (size_t k = 0; k < sizeof(kImports) / sizeof(kImports[0]); k++) {
            if (plain_strcmp(name, kImports[k].name) != 0)
                continue;
            if (g_slot_count < SK_MAX_SLOTS) {
                g_slots[g_slot_count].where = (uintptr_t *)(base + rela[i].r_offset);
                g_slots[g_slot_count].value = (uintptr_t)kImports[k].impl + rela[i].r_addend;
                g_slot_count++;
            }
            break;
        }
    }
}

static void scan_module(struct sk_mod_info *info)
{
    uintptr_t base = info->addr;
    const Elf64_Dyn *dyn = NULL;
    for (uint16_t i = 0; i < info->phnum; i++) {
        if (info->phdr[i].p_type == PT_DYNAMIC)
            dyn = (const Elf64_Dyn *)(base + info->phdr[i].p_vaddr);
    }
    if (!dyn)
        return;

    const Elf64_Sym *symtab = NULL;
    const char *strtab = NULL;
    const Elf64_Rela *rela = NULL, *jmprel = NULL;
    size_t relasz = 0, pltrelsz = 0;
    for (; dyn->d_tag != DT_NULL; dyn++) {
        switch (dyn->d_tag) {
        case DT_SYMTAB: symtab = (const Elf64_Sym *)dyn_ptr(base, dyn->d_un.d_ptr); break;
        case DT_STRTAB: strtab = (const char *)dyn_ptr(base, dyn->d_un.d_ptr); break;
        case DT_RELA: rela = (const Elf64_Rela *)dyn_ptr(base, dyn->d_un.d_ptr); break;
        case DT_RELASZ: relasz = dyn->d_un.d_val; break;
        case DT_JMPREL: jmprel = (const Elf64_Rela *)dyn_ptr(base, dyn->d_un.d_ptr); break;
        case DT_PLTRELSZ: pltrelsz = dyn->d_un.d_val; break;
        default: break;
        }
    }
    if (!symtab || !strtab)
        return;
    if (rela)
        scan_relocs(base, rela, relasz / sizeof(Elf64_Rela), symtab, strtab);
    if (jmprel)
        scan_relocs(base, jmprel, pltrelsz / sizeof(Elf64_Rela), symtab, strtab);
}

/* ------------------------------------------------- FMOD output */

/* Unity 2020.3 picks FMOD's OpenSL ES output unless the command line has
 * -disableOpenSL; the loader has no OpenSL ES (slCreateEngine is NULL) and
 * passes no command line, so FMOD fails and the game is silent. The
 * HasARGV("disableOpenSL") call in libunity is replaced by "true", so FMOD
 * uses the AudioTrack output the loader emulates. Only patched when the
 * instruction matches this exact libunity build. */
#define SK_UNITY_ARGV_CALL 0x45474cu
#define SK_UNITY_ARGV_ORIG 0x97fcfeb8u /* bl HasARGV */
#define SK_UNITY_ARGV_TRUE 0x52800020u /* mov w0, #1 */

static uintptr_t g_unity_base;
static int g_opensl_patched;

static void patch_disable_opensl(void)
{
    if (!g_unity_base || g_opensl_patched)
        return;
    g_opensl_patched = 1;
    uint32_t *insn = (uint32_t *)(g_unity_base + SK_UNITY_ARGV_CALL);
    if (*insn == SK_UNITY_ARGV_TRUE)
        return;
    if (*insn != SK_UNITY_ARGV_ORIG) {
        sk_log("[sk-shim] libunity build differs; OpenSL ES left enabled\n");
        return;
    }
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)insn & ~(uintptr_t)(page - 1);
    if (mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        sk_log("[sk-shim] cannot unprotect libunity code; OpenSL ES left enabled\n");
        return;
    }
    *insn = SK_UNITY_ARGV_TRUE;
    __builtin___clear_cache((char *)insn, (char *)(insn + 1));
    mprotect((void *)start, (size_t)page, PROT_READ | PROT_EXEC);
    sk_log("[sk-shim] FMOD: OpenSL ES disabled, using AudioTrack output\n");
}

static int on_module(struct sk_mod_info *info, size_t size, void *ctx)
{
    (void)size;
    (void)ctx;
    const char *name = base_name(info->name);
    if (!g_scanned_unity && plain_strcmp(name, "libunity.so") == 0) {
        g_scanned_unity = 1;
        g_unity_base = info->addr;
        scan_module(info);
    } else if (!g_scanned_il2cpp && plain_strcmp(name, "libil2cpp.so") == 0) {
        g_scanned_il2cpp = 1;
        scan_module(info);
    }
    return 0;
}

static void apply_slots(void)
{
    long page = sysconf(_SC_PAGESIZE);
    int written = 0;
    for (int i = 0; i < g_slot_count; i++) {
        if (*g_slots[i].where == g_slots[i].value)
            continue;
        uintptr_t start = (uintptr_t)g_slots[i].where & ~(uintptr_t)(page - 1);
        mprotect((void *)start, (size_t)page, PROT_READ | PROT_WRITE);
        *g_slots[i].where = g_slots[i].value;
        written++;
    }
    if (written)
        sk_log_num("[sk-shim] filled ", (unsigned long)written, " missing import slot(s)\n");
}

/* ------------------------------------------------- JNI handle guard */

/* The loader returns bare ids (e.g. 379) instead of objects for reflected
 * fields it does not know, and its dispatch() then dereferences them when
 * C# calls a method on one (PackageInfo.signatures crashed at 99% loading).
 * The instance Call<Type>Method{,V,A} and Get<Type>Field slots of the JNIEnv
 * table are wrapped: a handle below 64 KiB yields 0, like the loader's own
 * answer for unknown methods; anything else goes to the original slot. */

#define SK_JNI_SLOTS 233
#define SK_BAD_HANDLE(o) ((uintptr_t)(o) < 0x10000)

static void *g_jni_orig[SK_JNI_SLOTS];
static int g_jni_bad_logs;

static void jni_bad(int slot, const void *obj)
{
    if (g_jni_bad_logs < 20) {
        char buf[96];
        g_jni_bad_logs++;
        snprintf(buf, sizeof(buf), "[sk-shim] JNI slot %d on invalid handle %p -> 0\n",
                 slot, obj);
        sk_log(buf);
    }
}

typedef uint64_t (*sk_call_v)(void *, void *, void *, va_list);
typedef uint64_t (*sk_call_a)(void *, void *, void *, const void *);
typedef uint64_t (*sk_get)(void *, void *, void *);

/* i: the "..." slot of a Call<Type>Method triple; i+1 is V, i+2 is A. */
#define SK_CALL_INT(i)                                                       \
    static uint64_t sk_call_##i(void *e, void *o, void *m, ...)              \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i, o);                                                   \
            return 0;                                                        \
        }                                                                    \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        uint64_t r = ((sk_call_v)g_jni_orig[i + 1])(e, o, m, ap);            \
        va_end(ap);                                                          \
        return r;                                                            \
    }                                                                        \
    static uint64_t sk_callv_##i(void *e, void *o, void *m, va_list ap)      \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i + 1, o);                                               \
            return 0;                                                        \
        }                                                                    \
        return ((sk_call_v)g_jni_orig[i + 1])(e, o, m, ap);                  \
    }                                                                        \
    static uint64_t sk_calla_##i(void *e, void *o, void *m, const void *a)   \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i + 2, o);                                               \
            return 0;                                                        \
        }                                                                    \
        return ((sk_call_a)g_jni_orig[i + 2])(e, o, m, a);                   \
    }

#define SK_CALL_FP(i, T)                                                     \
    static T sk_call_##i(void *e, void *o, void *m, ...)                     \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i, o);                                                   \
            return 0;                                                        \
        }                                                                    \
        va_list ap;                                                          \
        va_start(ap, m);                                                     \
        T r = ((T(*)(void *, void *, void *, va_list))g_jni_orig[i + 1])(    \
            e, o, m, ap);                                                    \
        va_end(ap);                                                          \
        return r;                                                            \
    }                                                                        \
    static T sk_callv_##i(void *e, void *o, void *m, va_list ap)             \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i + 1, o);                                               \
            return 0;                                                        \
        }                                                                    \
        return ((T(*)(void *, void *, void *, va_list))g_jni_orig[i + 1])(   \
            e, o, m, ap);                                                    \
    }                                                                        \
    static T sk_calla_##i(void *e, void *o, void *m, const void *a)          \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i + 2, o);                                               \
            return 0;                                                        \
        }                                                                    \
        return ((T(*)(void *, void *, void *, const void *))g_jni_orig[i + 2])( \
            e, o, m, a);                                                     \
    }

#define SK_GET_INT(i)                                                        \
    static uint64_t sk_get_##i(void *e, void *o, void *f)                    \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i, o);                                                   \
            return 0;                                                        \
        }                                                                    \
        return ((sk_get)g_jni_orig[i])(e, o, f);                             \
    }

#define SK_GET_FP(i, T)                                                      \
    static T sk_get_##i(void *e, void *o, void *f)                           \
    {                                                                        \
        if (SK_BAD_HANDLE(o)) {                                              \
            jni_bad(i, o);                                                   \
            return 0;                                                        \
        }                                                                    \
        return ((T(*)(void *, void *, void *))g_jni_orig[i])(e, o, f);       \
    }

/* Object, Boolean, Byte, Char, Short, Int, Long, Float, Double, Void. */
SK_CALL_INT(34) SK_CALL_INT(37) SK_CALL_INT(40) SK_CALL_INT(43) SK_CALL_INT(46)
SK_CALL_INT(49) SK_CALL_INT(52) SK_CALL_FP(55, float) SK_CALL_FP(58, double) SK_CALL_INT(61)
SK_GET_INT(95) SK_GET_INT(96) SK_GET_INT(97) SK_GET_INT(98) SK_GET_INT(99)
SK_GET_INT(100) SK_GET_INT(101) SK_GET_FP(102, float) SK_GET_FP(103, double)

#define SK_TRIPLE(i) { i, (void *)sk_call_##i }, { i + 1, (void *)sk_callv_##i }, \
                     { i + 2, (void *)sk_calla_##i }
#define SK_GETTER(i) { i, (void *)sk_get_##i }

static const struct {
    int slot;
    void *wrapper;
} kJniWraps[] = {
    SK_TRIPLE(34), SK_TRIPLE(37), SK_TRIPLE(40), SK_TRIPLE(43), SK_TRIPLE(46),
    SK_TRIPLE(49), SK_TRIPLE(52), SK_TRIPLE(55), SK_TRIPLE(58), SK_TRIPLE(61),
    SK_GETTER(95), SK_GETTER(96), SK_GETTER(97), SK_GETTER(98), SK_GETTER(99),
    SK_GETTER(100), SK_GETTER(101), SK_GETTER(102), SK_GETTER(103),
};

static int g_jni_wrapped;

static void wrap_jni_table(void)
{
    void *(*env_fn)(void) = (void *(*)(void))dlsym(RTLD_DEFAULT, "bc_jni_env");
    if (!env_fn)
        return;
    void ***env = (void ***)env_fn();
    if (!env || !*env)
        return;
    void **table = *env;
    for (size_t k = 0; k < sizeof(kJniWraps) / sizeof(kJniWraps[0]); k++) {
        if (!table[kJniWraps[k].slot])
            return; /* table not filled in yet */
    }
    long page = sysconf(_SC_PAGESIZE);
    uintptr_t start = (uintptr_t)table & ~(uintptr_t)(page - 1);
    uintptr_t end = (uintptr_t)(table + SK_JNI_SLOTS);
    mprotect((void *)start, end - start, PROT_READ | PROT_WRITE);
    for (size_t k = 0; k < sizeof(kJniWraps) / sizeof(kJniWraps[0]); k++) {
        int slot = kJniWraps[k].slot;
        g_jni_orig[slot] = table[slot];
    }
    for (size_t k = 0; k < sizeof(kJniWraps) / sizeof(kJniWraps[0]); k++)
        table[kJniWraps[k].slot] = kJniWraps[k].wrapper;
    g_jni_wrapped = 1;
    sk_log("[sk-shim] JNIEnv Call*Method/Get*Field slots guarded\n");
}

static void sk_fixup(void)
{
    static sk_iterate_fn iterate;
    static int lookup_done;

    while (__sync_lock_test_and_set(&g_lock, 1))
        sched_yield();
    if (!lookup_done) {
        lookup_done = 1;
        iterate = (sk_iterate_fn)dlsym(RTLD_DEFAULT, "bc_iterate_mods");
        if (!iterate)
            sk_log("[sk-shim] bc_iterate_mods not found; imports not patched\n");
    }
    if (iterate && (!g_scanned_unity || !g_scanned_il2cpp)) {
        iterate(on_module, NULL);
        if (g_scanned_unity && g_scanned_il2cpp)
            sk_log_num("[sk-shim] found ", (unsigned long)g_slot_count, " missing import slot(s)\n");
    }
    apply_slots();
    patch_disable_opensl();
    if (!g_jni_wrapped)
        wrap_jni_table();
    __sync_lock_release(&g_lock);
}

int pthread_create(pthread_t *thread, const pthread_attr_t *attr,
                   void *(*start)(void *), void *arg)
{
    static int (*real)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
    if (!real)
        real = (int (*)(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *))
            dlsym(RTLD_NEXT, "pthread_create");
    sk_fixup();
    return real(thread, attr, start, arg);
}

/* ------------------------------------------------- path mapping */

/* Two kinds of paths come out wrong under this loader and are mapped back
 * into the game dir (the loader's cwd):
 *  - StreamingAssets: Unity's APK root is "", so streamingAssetsPath is
 *    "jar:file://!/assets" and its files resolve to "/assets/..." ->
 *    "assets/...".
 *  - Save data: the game asks UnityPlayerActivity.getFilesDir() through
 *    reflection, the loader answers with a stub, and persistentDataPath ends
 *    up "" -> item_data.data, statistic.data, ... land in "/" of the handheld.
 *    "/<name>.data", "/<name>.bytes" and the game's own folders -> "home/..."
 *    (where getFilesDir() and PlayerPrefs already live). */

static const char kJarRoot[] = "jar:file://!/";
static const char kAbsAssets[] = "/assets/";
static const char *const kHomeDirs[] = {
    "VersionConfigs", "Config", "dynamic_config", "HotfixPatches", "chilly_texture",
    "chilly_promotion", "RealName", "Strategy", "chat", NULL,
};
static int g_path_logs;

static int has_prefix(const char *s, const char *prefix)
{
    while (*prefix) {
        if (*s++ != *prefix++)
            return 0;
    }
    return 1;
}

static int has_suffix(const char *s, size_t len, const char *suffix)
{
    size_t n = strlen(suffix);
    return len >= n && memcmp(s + len - n, suffix, n) == 0;
}

static int is_home_path(const char *path)
{
    if (path[0] != '/' || path[1] == '\0')
        return 0;
    const char *name = path + 1;
    const char *slash = strchr(name, '/');
    size_t len = slash ? (size_t)(slash - name) : strlen(name);
    if (!slash && (has_suffix(name, len, ".data") || has_suffix(name, len, ".bytes")))
        return 1;
    for (int i = 0; kHomeDirs[i]; i++) {
        if (strlen(kHomeDirs[i]) == len && memcmp(name, kHomeDirs[i], len) == 0)
            return 1;
    }
    return 0;
}

static int mentions_bundles(const char *s)
{
    return strstr(s, "AssetBundles") != NULL;
}

/* Returns the path to use, built in buf when it has to change. */
static const char *map_path(const char *path, const char *what, char *buf, size_t size)
{
    if (!path)
        return path;
    const char *mapped = path;
    if (has_prefix(path, kJarRoot))
        mapped = path + sizeof(kJarRoot) - 1;
    else if (has_prefix(path, kAbsAssets))
        mapped = path + 1;
    else if (is_home_path(path) && (size_t)snprintf(buf, size, "home%s", path) < size)
        mapped = buf;
    if (g_path_logs < 60 && (mapped != path || mentions_bundles(path))) {
        char line[600];
        g_path_logs++;
        snprintf(line, sizeof(line), "[sk-shim] %s(\"%s\")%s%s\n", what, path,
                 mapped != path ? " -> " : "", mapped != path ? mapped : "");
        sk_log(line);
    }
    return mapped;
}

#define SK_PATH_BUF(path) char path##_buf[4096]
#define SK_MAP(path, what) map_path((path), (what), path##_buf, sizeof(path##_buf))

static volatile int g_first_seen[8];

static void sk_first_call(int slot, const char *name)
{
    if (!g_first_seen[slot]) {
        char buf[96];
        g_first_seen[slot] = 1;
        snprintf(buf, sizeof(buf), "[sk-shim] first %s() call intercepted\n", name);
        sk_log(buf);
    }
}

#define SK_REAL(type, name, args) \
    static type(*real) args;      \
    if (!real)                    \
        real = (type(*) args)dlsym(RTLD_NEXT, name)

#define SK_OPEN_MODE(flags, last)              \
    mode_t mode = 0;                           \
    if ((flags) & (O_CREAT | O_TMPFILE)) {     \
        va_list ap;                            \
        va_start(ap, last);                    \
        mode = (mode_t)va_arg(ap, int);        \
        va_end(ap);                            \
    }

int open(const char *path, int flags, ...)
{
    SK_REAL(int, "open", (const char *, int, ...));
    SK_PATH_BUF(path);
    sk_first_call(3, "open");
    SK_OPEN_MODE(flags, flags);
    return real(SK_MAP(path, "open"), flags, mode);
}

int open64(const char *path, int flags, ...)
{
    SK_REAL(int, "open64", (const char *, int, ...));
    SK_PATH_BUF(path);
    sk_first_call(6, "open64");
    SK_OPEN_MODE(flags, flags);
    return real(SK_MAP(path, "open64"), flags, mode);
}

int openat(int dirfd, const char *path, int flags, ...)
{
    SK_REAL(int, "openat", (int, const char *, int, ...));
    SK_PATH_BUF(path);
    sk_first_call(5, "openat");
    SK_OPEN_MODE(flags, flags);
    return real(dirfd, SK_MAP(path, "openat"), flags, mode);
}

FILE *fopen(const char *path, const char *fmode)
{
    SK_REAL(FILE *, "fopen", (const char *, const char *));
    SK_PATH_BUF(path);
    sk_first_call(4, "fopen");
    return real(SK_MAP(path, "fopen"), fmode);
}

FILE *fopen64(const char *path, const char *fmode)
{
    SK_REAL(FILE *, "fopen64", (const char *, const char *));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "fopen64"), fmode);
}

/* glibc >= 2.33 keeps __xstat & co. only as compat symbols, which
 * dlsym(RTLD_NEXT) cannot find, so these go straight to the syscall. The
 * aarch64 kernel struct stat is the one glibc uses. */
static int stat_syscall(int dirfd, const char *path, struct stat *st, int flags)
{
    return (int)syscall(SYS_newfstatat, dirfd, path, st, flags);
}

int __xstat(int ver, const char *path, struct stat *st)
{
    SK_PATH_BUF(path);
    (void)ver;
    sk_first_call(0, "__xstat");
    return stat_syscall(AT_FDCWD, SK_MAP(path, "stat"), st, 0);
}

int __lxstat(int ver, const char *path, struct stat *st)
{
    SK_PATH_BUF(path);
    (void)ver;
    sk_first_call(1, "__lxstat");
    return stat_syscall(AT_FDCWD, SK_MAP(path, "lstat"), st, AT_SYMLINK_NOFOLLOW);
}

int __fxstatat(int ver, int dirfd, const char *path, struct stat *st, int flags)
{
    SK_PATH_BUF(path);
    (void)ver;
    sk_first_call(2, "__fxstatat");
    return stat_syscall(dirfd, SK_MAP(path, "fstatat"), st, flags);
}

int access(const char *path, int amode)
{
    SK_REAL(int, "access", (const char *, int));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "access"), amode);
}

int faccessat(int dirfd, const char *path, int amode, int flags)
{
    SK_REAL(int, "faccessat", (int, const char *, int, int));
    SK_PATH_BUF(path);
    return real(dirfd, SK_MAP(path, "faccessat"), amode, flags);
}

DIR *opendir(const char *path)
{
    SK_REAL(DIR *, "opendir", (const char *));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "opendir"));
}

/* Saving writes a temp file, then renames or deletes, and creates folders. */

int mkdir(const char *path, mode_t mode)
{
    SK_REAL(int, "mkdir", (const char *, mode_t));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "mkdir"), mode);
}

int rmdir(const char *path)
{
    SK_REAL(int, "rmdir", (const char *));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "rmdir"));
}

int unlink(const char *path)
{
    SK_REAL(int, "unlink", (const char *));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "unlink"));
}

int remove(const char *path)
{
    SK_REAL(int, "remove", (const char *));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "remove"));
}

int rename(const char *from, const char *to)
{
    SK_REAL(int, "rename", (const char *, const char *));
    SK_PATH_BUF(from);
    SK_PATH_BUF(to);
    return real(SK_MAP(from, "rename"), SK_MAP(to, "rename"));
}

int truncate(const char *path, off_t length)
{
    SK_REAL(int, "truncate", (const char *, off_t));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "truncate"), length);
}

int chmod(const char *path, mode_t mode)
{
    SK_REAL(int, "chmod", (const char *, mode_t));
    SK_PATH_BUF(path);
    return real(SK_MAP(path, "chmod"), mode);
}

__attribute__((constructor)) static void sk_init(void)
{
    sk_log("[sk-shim] loaded (v7: imports + asset/save path map + JNI handle guard"
           " + FMOD AudioTrack)\n");
}
