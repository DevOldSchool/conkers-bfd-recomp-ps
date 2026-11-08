// Relative paths for the PS5 title, linked in by tools/ps5/link-title.sh.
//
// A title has no working directory: the kernel answers a relative path with EINVAL, and the console's
// getcwd is an import that resolves to nothing. The game, libc++ (std::filesystem) and the libraries
// use relative paths all the same. Here the title keeps a working directory of its own, starting at the
// title folder, /app0: a relative path is joined to it, with "." and ".." resolved as text, and an
// absolute path passes through untouched.
//
// The link wraps the C library's path functions (lld's --wrap=<name>): the title's references to
// <name> come here as __wrap_<name>, and __real_<name> is the C library's. opendir and access are the
// SDK platform layer's (PS5_Vulkan's link recipe binds them), which open and stat the path: wrapped
// here, so relative paths work for them too. Nothing here defines the
// system's own names, which system modules import (getcwd, lstat, access): a title that defined them
// would export them, and the title converter refuses that.
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define TITLE_FOLDER "/app0"

int __real_stat(const char* path, struct stat* sb);
int __real_lstat(const char* path, struct stat* sb);
int __real_open(const char* path, int flags, ...);
FILE* __real_fopen(const char* path, const char* mode);
int __real_mkdir(const char* path, mode_t mode);
int __real_rmdir(const char* path);
int __real_unlink(const char* path);
int __real_remove(const char* path);
int __real_rename(const char* from, const char* to);
int __real_truncate(const char* path, off_t length);
int __real_chmod(const char* path, mode_t mode);
char* __real_realpath(const char* path, char* resolved);

static pthread_mutex_t g_cwd_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_cwd[PATH_MAX] = TITLE_FOLDER;

// Appends one path component to an absolute path held in out (at least "/"), resolving "." and "..".
// Returns 0, or -1 when the result would not fit.
static int append_component(char* out, size_t* length, const char* component, size_t component_length) {
    if (component_length == 0 || (component_length == 1 && component[0] == '.')) {
        return 0;
    }
    if (component_length == 2 && component[0] == '.' && component[1] == '.') {
        // Up one, but never above the root.
        while (*length > 1 && out[*length - 1] != '/') {
            (*length)--;
        }
        if (*length > 1) {
            (*length)--;
        }
        out[*length] = '\0';
        return 0;
    }
    const size_t separator = (*length > 1) ? 1 : 0;
    if (*length + separator + component_length >= PATH_MAX) {
        return -1;
    }
    if (separator) {
        out[(*length)++] = '/';
    }
    memcpy(out + *length, component, component_length);
    *length += component_length;
    out[*length] = '\0';
    return 0;
}

// Resolves every component of path onto out, which holds an absolute path already.
static int append_path(char* out, size_t* length, const char* path) {
    while (*path != '\0') {
        while (*path == '/') {
            path++;
        }
        const char* end = path;
        while (*end != '\0' && *end != '/') {
            end++;
        }
        if (append_component(out, length, path, (size_t)(end - path)) != 0) {
            return -1;
        }
        path = end;
    }
    return 0;
}

// The path to give the C library for path: path itself when it is absolute (or empty, which fails
// as it would anyway), else the working directory joined with it, in buffer. NULL with errno set
// when it does not fit.
static const char* resolve(const char* path, char* buffer) {
    if (path == NULL || path[0] == '/' || path[0] == '\0') {
        return path;
    }
    pthread_mutex_lock(&g_cwd_lock);
    strcpy(buffer, g_cwd);
    pthread_mutex_unlock(&g_cwd_lock);
    size_t length = strlen(buffer);
    if (append_path(buffer, &length, path) != 0) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return buffer;
}

#define RESOLVE_OR_FAIL(name, path, failure) \
    char name##_buffer[PATH_MAX];            \
    const char* name = resolve(path, name##_buffer); \
    if (name == NULL) {                      \
        return failure;                      \
    }

int __wrap_chdir(const char* path) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    struct stat sb;
    if (__real_stat(resolved, &sb) != 0) {
        return -1;
    }
    if (!S_ISDIR(sb.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }
    // Kept as text, "." and ".." already resolved, so later joins stay short.
    char normalised[PATH_MAX] = "/";
    size_t length = 1;
    if (append_path(normalised, &length, resolved) != 0) {
        errno = ENAMETOOLONG;
        return -1;
    }
    pthread_mutex_lock(&g_cwd_lock);
    strcpy(g_cwd, normalised);
    pthread_mutex_unlock(&g_cwd_lock);
    return 0;
}

// The working directory this layer keeps: the console's own getcwd is missing.
char* __wrap_getcwd(char* buffer, size_t size) {
    pthread_mutex_lock(&g_cwd_lock);
    const size_t length = strlen(g_cwd);
    if (buffer == NULL) {
        // As FreeBSD's: allocated when no buffer is given (size is then a minimum, 0 meaning any).
        if (size < length + 1) {
            size = length + 1;
        }
        buffer = malloc(size);
        if (buffer == NULL) {
            pthread_mutex_unlock(&g_cwd_lock);
            errno = ENOMEM;
            return NULL;
        }
    } else if (size == 0) {
        pthread_mutex_unlock(&g_cwd_lock);
        errno = EINVAL;
        return NULL;
    } else if (size < length + 1) {
        pthread_mutex_unlock(&g_cwd_lock);
        errno = ERANGE;
        return NULL;
    }
    memcpy(buffer, g_cwd, length + 1);
    pthread_mutex_unlock(&g_cwd_lock);
    return buffer;
}

int __wrap_stat(const char* path, struct stat* sb) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_stat(resolved, sb);
}

int __wrap_lstat(const char* path, struct stat* sb) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_lstat(resolved, sb);
}

int __wrap_open(const char* path, int flags, ...) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    // The mode is there only when a file may be created.
    if ((flags & O_CREAT) != 0) {
        va_list arguments;
        va_start(arguments, flags);
        const int mode = va_arg(arguments, int);
        va_end(arguments);
        return __real_open(resolved, flags, mode);
    }
    return __real_open(resolved, flags);
}

FILE* __wrap_fopen(const char* path, const char* mode) {
    RESOLVE_OR_FAIL(resolved, path, NULL);
    return __real_fopen(resolved, mode);
}

int __wrap_mkdir(const char* path, mode_t mode) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_mkdir(resolved, mode);
}

int __wrap_rmdir(const char* path) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_rmdir(resolved);
}

int __wrap_unlink(const char* path) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_unlink(resolved);
}

int __wrap_remove(const char* path) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_remove(resolved);
}

int __wrap_rename(const char* from, const char* to) {
    RESOLVE_OR_FAIL(resolved_from, from, -1);
    RESOLVE_OR_FAIL(resolved_to, to, -1);
    return __real_rename(resolved_from, resolved_to);
}

int __wrap_truncate(const char* path, off_t length) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_truncate(resolved, length);
}

int __wrap_chmod(const char* path, mode_t mode) {
    RESOLVE_OR_FAIL(resolved, path, -1);
    return __real_chmod(resolved, mode);
}

// The C library's realpath on the absolute path, so the path it resolves exists, as realpath's must.
char* __wrap_realpath(const char* path, char* resolved_path) {
    RESOLVE_OR_FAIL(resolved, path, NULL);
    return __real_realpath(resolved, resolved_path);
}
