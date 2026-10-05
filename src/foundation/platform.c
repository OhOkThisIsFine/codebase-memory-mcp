/*
 * platform.c — OS abstraction implementations.
 *
 * macOS, Linux, and Windows. Platform-specific code behind #ifdef guards.
 */
#include "platform.h"

#include "foundation/compat.h"
#include "foundation/constants.h"
#include "foundation/platform_internal.h"
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CBM_NSEC_PER_SEC 1000000000ULL

static uint64_t cbm_platform_scale_fraction(uint64_t remainder, uint64_t multiplier,
                                            uint64_t divisor) {
    uint64_t quotient = 0;
    uint64_t reduced = 0;
    uint64_t mask = 1;
    while (mask <= multiplier / 2) {
        mask <<= 1U;
    }
    for (; mask != 0; mask >>= 1U) {
        quotient *= 2;
        if (reduced >= divisor - reduced) {
            reduced -= divisor - reduced;
            quotient++;
        } else {
            reduced += reduced;
        }
        if ((multiplier & mask) != 0) {
            if (reduced >= divisor - remainder) {
                reduced -= divisor - remainder;
                quotient++;
            } else {
                reduced += remainder;
            }
        }
    }
    return quotient;
}

uint64_t cbm_platform_scale_counter_ns(uint64_t counter, uint64_t frequency) {
    if (frequency == 0) {
        return UINT64_MAX;
    }
    uint64_t whole_seconds = counter / frequency;
    uint64_t remainder = counter % frequency;
    if (whole_seconds > UINT64_MAX / CBM_NSEC_PER_SEC) {
        return UINT64_MAX;
    }
    uint64_t whole_ns = whole_seconds * CBM_NSEC_PER_SEC;
    uint64_t fraction_ns = cbm_platform_scale_fraction(remainder, CBM_NSEC_PER_SEC, frequency);
    return fraction_ns > UINT64_MAX - whole_ns ? UINT64_MAX : whole_ns + fraction_ns;
}

/* Canonicalize a Windows drive letter to upper-case in place: "c:/x" -> "C:/x".
 * Windows drive letters are case-insensitive, but a lowercase one (as agent
 * CWDs often report, e.g. Claude Code's "c:\...") otherwise produces a distinct
 * project key ("c-..." vs "C-...") and, on a case-insensitive FS, a colliding
 * cache file that clobbers the good index (#227/#367/#394). Folding to a single
 * canonical form here — at the one path-normalization choke point — keeps the
 * project key, cache file and integrity check consistent regardless of case.
 * Only the strict drive-root form `X:/` or bare `X:` is touched, so ordinary
 * POSIX paths (which never start that way) are unaffected. */
static void cbm_canonicalize_drive(char *path) {
    if (path && path[0] >= 'a' && path[0] <= 'z' && path[1] == ':' &&
        (path[2] == '/' || path[2] == '\0')) {
        path[0] = (char)(path[0] - 'a' + 'A');
    }
}

#ifdef _WIN32

/* ── Windows implementation ────────────────────────────────── */

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <io.h>
#include <sys/stat.h>
#include "foundation/win_utf8.h"

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return NULL;
    }

    HANDLE file = CreateFileW(wpath, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) {
        free(wpath);
        return NULL;
    }
    LARGE_INTEGER sz;
    if (!GetFileSizeEx(file, &sz) || sz.QuadPart == 0) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    HANDLE mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    if (!mapping) {
        CloseHandle(file);
        free(wpath);
        return NULL;
    }
    void *addr = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    CloseHandle(mapping);
    CloseHandle(file);
    free(wpath);
    if (!addr) {
        return NULL;
    }
    *out_size = (size_t)sz.QuadPart;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    (void)size;
    if (addr) {
        UnmapViewOfFile(addr);
    }
}

uint64_t cbm_now_ns(void) {
    LARGE_INTEGER freq, count;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    return cbm_platform_scale_counter_ns((uint64_t)count.QuadPart, (uint64_t)freq.QuadPart);
}

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

int cbm_nprocs(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    return (int)si.dwNumberOfProcessors > 0 ? (int)si.dwNumberOfProcessors : 1;
}

bool cbm_file_exists(const char *path) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES;
}

bool cbm_is_dir(const char *path) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return false;
    }
    DWORD attr = GetFileAttributesW(wpath);
    free(wpath);
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

int64_t cbm_file_size(const char *path) {
    wchar_t *wpath = cbm_path_to_wide(path);
    if (!wpath) {
        return CBM_NOT_FOUND;
    }
    WIN32_FILE_ATTRIBUTE_DATA fad;
    BOOL ok = GetFileAttributesExW(wpath, GetFileExInfoStandard, &fad);
    free(wpath);
    if (!ok) {
        return CBM_NOT_FOUND;
    }
    LARGE_INTEGER sz;
    sz.HighPart = (LONG)fad.nFileSizeHigh; // cppcheck-suppress unreadVariable
    sz.LowPart = fad.nFileSizeLow;         // cppcheck-suppress unreadVariable
    return (int64_t)sz.QuadPart;
}

char *cbm_normalize_path_sep(char *path) {
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#else /* POSIX (macOS + Linux) */

/* ── POSIX implementation ────────────────────────────────── */

#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

#ifdef __APPLE__
#include <mach/mach_time.h>
#include <pthread.h>
#include <sys/sysctl.h>
#else
#include <sched.h>
#endif

/* ── Memory mapping ──────────────────────────── */

void *cbm_mmap_read(const char *path, size_t *out_size) {
    if (!path || !out_size) {
        return NULL;
    }
    *out_size = 0;

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        return NULL;
    }

    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size == 0) {
        close(fd);
        return NULL;
    }

    void *addr = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);

    if (addr == MAP_FAILED) {
        return NULL;
    }
    *out_size = (size_t)st.st_size;
    return addr;
}

void cbm_munmap(void *addr, size_t size) {
    if (addr && size > 0) {
        munmap(addr, size);
    }
}

/* ── Timing ───────────────────────────── */

#ifdef __APPLE__
static mach_timebase_info_data_t timebase_info = {1, 1};
static pthread_once_t timebase_once = PTHREAD_ONCE_INIT;

static void cbm_timebase_initialize(void) {
    (void)mach_timebase_info(&timebase_info);
    if (timebase_info.numer == 0 || timebase_info.denom == 0) {
        timebase_info.numer = 1;
        timebase_info.denom = 1;
    }
}

uint64_t cbm_now_ns(void) {
    (void)pthread_once(&timebase_once, cbm_timebase_initialize);
    uint64_t ticks = mach_absolute_time();
    return ticks * timebase_info.numer / timebase_info.denom;
}
#else
uint64_t cbm_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}
#endif

#define CBM_USEC_PER_SEC 1000000ULL

uint64_t cbm_now_ms(void) {
    return cbm_now_ns() / CBM_USEC_PER_SEC;
}

/* ── System info ───────────────────────────── */

int cbm_nprocs(void) {
#ifdef __APPLE__
    int ncpu = 0;
    size_t len = sizeof(ncpu);
    if (sysctlbyname("hw.ncpu", &ncpu, &len, NULL, 0) == 0 && ncpu > 0) {
        return ncpu;
    }
    enum { FILE_EXISTS = 1 };
    return FILE_EXISTS;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    return n > 0 ? (int)n : 1;
#endif
}

/* ── File system ──────────────────────────── */

bool cbm_file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

bool cbm_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int64_t cbm_file_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0) {
        return CBM_NOT_FOUND;
    }
    return (int64_t)st.st_size;
}

char *cbm_normalize_path_sep(char *path) {
    /* Normalize on ALL platforms — backslash paths can arrive via stored
     * data, cross-platform DB files, or Windows-style arguments. */
    if (path) {
        for (char *p = path; *p; p++) {
            if (*p == '\\') {
                *p = '/';
            }
        }
        cbm_canonicalize_drive(path);
    }
    return path;
}

#endif /* _WIN32 */

/* ── Environment variables ──────────────────────────── */

/* Thread-safe getenv: iterates environ directly instead of calling getenv().
 * getenv() is flagged by concurrency-mt-unsafe because the returned pointer
 * can be invalidated by setenv/putenv in another thread. We copy to a
 * caller-owned buffer immediately. */
#ifdef _WIN32
#include <stdlib.h>
#define CBM_ENVIRON _environ
#elif defined(__APPLE__)
#include <crt_externs.h>
#define CBM_ENVIRON (*_NSGetEnviron())
#else
extern char **environ;
#define CBM_ENVIRON environ
#endif

static const char *platform_copy_environment_value(char *buf, size_t buf_sz, const char *value) {
    if (!buf || buf_sz == 0 || !value) {
        return NULL;
    }
    size_t length = strlen(value);
    if (length >= buf_sz) {
        buf[0] = '\0';
        return NULL;
    }
    memcpy(buf, value, length + 1U);
    return buf;
}

const char *cbm_safe_getenv(const char *name, char *buf, size_t buf_sz, const char *fallback) {
    if (!name || !name[0] || !buf || buf_sz == 0) {
        return NULL;
    }
    buf[0] = '\0';
#ifdef _WIN32
    /* #996 Layer 2: _environ holds ANSI-code-page bytes, NOT UTF-8. A
     * non-ASCII value (USERPROFILE of C:\Users\Kovács János, or a Greek/CJK
     * CBM_CACHE_DIR) arrives here either mojibake'd or with unrepresentable
     * characters replaced by '?', which is INVALID in Windows paths — every
     * downstream wide-safe file API then fails no matter how correct it is.
     * Read the value wide and convert to genuine UTF-8, matching the
     * UTF-8-path convention the rest of the codebase (cbm_fopen, _wmkdir,
     * SQLite VFS) already assumes. */
    {
        wchar_t wname[CBM_SZ_256];
        int wn = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, wname, CBM_SZ_256);
        if (wn > 0) {
            SetLastError(ERROR_SUCCESS);
            DWORD needed = GetEnvironmentVariableW(wname, NULL, 0U);
            DWORD environment_error = GetLastError();
            if (needed == 0U) {
                if (environment_error == ERROR_ENVVAR_NOT_FOUND) {
                    return fallback ? platform_copy_environment_value(buf, buf_sz, fallback) : NULL;
                }
                if (environment_error != ERROR_SUCCESS) {
                    return NULL;
                }
                /* An existing empty variable is distinct from a missing one. */
                return buf;
            }
            wchar_t *wval = calloc((size_t)needed, sizeof(*wval));
            if (!wval) {
                return NULL;
            }
            SetLastError(ERROR_SUCCESS);
            DWORD got = GetEnvironmentVariableW(wname, wval, needed);
            DWORD read_error = GetLastError();
            if (got >= needed || (got == 0U && read_error != ERROR_SUCCESS)) {
                free(wval);
                return NULL;
            }
            char *utf8 = cbm_wide_to_utf8(wval);
            free(wval);
            if (!utf8) {
                return NULL;
            }
            const char *copied = platform_copy_environment_value(buf, buf_sz, utf8);
            free(utf8);
            return copied;
        }
        return NULL;
    }
#else
    char **env = CBM_ENVIRON;
    if (env) {
        size_t nlen = strlen(name);
        for (; *env; env++) {
            if (strncmp(*env, name, nlen) == 0 && (*env)[nlen] == '=') {
                return platform_copy_environment_value(buf, buf_sz, *env + nlen + SKIP_ONE);
            }
        }
    }
#endif
    if (fallback) {
        return platform_copy_environment_value(buf, buf_sz, fallback);
    }
    return NULL;
}

/* ── Home directory (cross-platform) ───────────────────── */

/* A usable home directory is an ABSOLUTE path. Two shapes are rejected:
 *
 *  - a relative value ("foo", "./foo"), and
 *  - an UNEXPANDED Windows-style token ("%USERPROFILE%", "~", "%HOMEDRIVE%%HOMEPATH%").
 *
 * Both are accepted verbatim by the environment lookup, and both are then
 * joined into ".../.cache/codebase-memory-mcp". Because the joined path is
 * relative, the daemon's cbm_mkdir_p() resolves it against the process's
 * current working directory, silently materializing a literal "%USERPROFILE%"
 * directory next to whatever cwd the launching hook happened to have
 * (observed under ~/.claude/hooks and ~/.agent-config). Treating such a value
 * as "unset" makes callers fall through to the real variable or fail loudly
 * instead of writing a stray cache tree. */
bool cbm_home_dir_value_usable(const char *value) {
    if (!value || !value[0] || value[0] == '%' || value[0] == '~') {
        return false;
    }
#ifdef _WIN32
    bool drive_letter =
        (value[0] >= 'a' && value[0] <= 'z') || (value[0] >= 'A' && value[0] <= 'Z');
    if (drive_letter && value[1] == ':' && (value[2] == '/' || value[2] == '\\')) {
        return true;
    }
    /* UNC requires both a server and share. Root-relative paths depend on
     * the current drive. Device namespaces are not home identities. */
    if (!((value[0] == '/' && value[1] == '/') || (value[0] == '\\' && value[1] == '\\'))) {
        return false;
    }
    const char *server = value + 2;
    if (!server[0] || server[0] == '/' || server[0] == '\\' || server[0] == '?' ||
        server[0] == '.') {
        return false;
    }
    const char *share = server;
    while (*share && *share != '/' && *share != '\\')
        share++;
    if (!*share)
        return false;
    share++;
    const char *end = share;
    while (*end && *end != '/' && *end != '\\')
        end++;
    size_t length = (size_t)(end - share);
    return length > 0 && !(length == 1 && share[0] == '.') &&
           !(length == 2 && share[0] == '.' && share[1] == '.');
#else
    return value[0] == '/';
#endif
}

const char *cbm_get_home_dir_checked(bool *read_failed) {
    static CBM_TLS char buf[CBM_SZ_4K];
    const char *names[] = {"HOME", "USERPROFILE"};
    *read_failed = false;
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        /* Empty fallback distinguishes absence from overflow/read failure.
         * A failed higher-priority read must not select a different identity. */
        if (!cbm_safe_getenv(names[i], buf, sizeof(buf), "")) {
            *read_failed = true;
            return NULL;
        }
        if (cbm_home_dir_value_usable(buf)) {
#ifdef _WIN32
            cbm_normalize_path_sep(buf);
#endif
            return buf;
        }
    }
    return NULL;
}

const char *cbm_get_home_dir(void) {
    bool read_failed;
    return cbm_get_home_dir_checked(&read_failed);
}

/* ── App config directories (cross-platform) ────────── */

static const char *platform_app_dir(const char *variable, const char *suffix, char *buf,
                                    size_t size) {
    if (!cbm_safe_getenv(variable, buf, size, "")) {
        return NULL;
    }
    if (buf[0]) {
#ifdef _WIN32
        cbm_normalize_path_sep(buf);
#endif
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    int written = snprintf(buf, size, "%s%s", home, suffix);
    if (written <= 0 || (size_t)written >= size) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}

const char *cbm_app_config_dir(void) {
    static CBM_TLS char buf[CBM_SZ_4K];
#ifdef _WIN32
    return platform_app_dir("APPDATA", "/AppData/Roaming", buf, sizeof(buf));
#else
    /* Linux: XDG_CONFIG_HOME or ~/.config */
    return platform_app_dir("XDG_CONFIG_HOME", "/.config", buf, sizeof(buf));
#endif /* _WIN32 */
}

const char *cbm_app_local_dir(void) {
#ifdef _WIN32
    static CBM_TLS char buf[CBM_SZ_4K];
    return platform_app_dir("LOCALAPPDATA", "/AppData/Local", buf, sizeof(buf));
#else
    return cbm_app_config_dir();
#endif
}

/* ── Cache directory ────────────────────────── */

const char *cbm_resolve_cache_dir(void) {
    static CBM_TLS char buf[CBM_SZ_4K];
    static const char missing[] = "\x1f"
                                  "CBM_CACHE_DIR_MISSING"
                                  "\x1f";
    const char *configured = cbm_safe_getenv("CBM_CACHE_DIR", buf, sizeof(buf), missing);
    if (!configured) {
        /* Present but not representable in the product path bound. */
        return NULL;
    }
    if (strcmp(configured, missing) != 0 && configured[0]) {
#ifdef _WIN32
        cbm_normalize_path_sep(buf);
#endif
        return buf;
    }
    const char *home = cbm_get_home_dir();
    if (!home) {
        return NULL;
    }
    int written = snprintf(buf, sizeof(buf), "%s/.cache/codebase-memory-mcp", home);
    if (written <= 0 || (size_t)written >= sizeof(buf)) {
        buf[0] = '\0';
        return NULL;
    }
    return buf;
}
