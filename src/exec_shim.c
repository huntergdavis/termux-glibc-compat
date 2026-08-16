#define _GNU_SOURCE

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

#if defined(__aarch64__)
#define TGCOMPAT_ELF_MACHINE EM_AARCH64
static const char *const default_interpreters[] = {
    "/lib/ld-linux-aarch64.so.1",
    "/lib64/ld-linux-aarch64.so.1",
    NULL,
};
#elif defined(__x86_64__)
#define TGCOMPAT_ELF_MACHINE EM_X86_64
static const char *const default_interpreters[] = {
    "/lib64/ld-linux-x86-64.so.2",
    "/lib/x86_64-linux-gnu/ld-linux-x86-64.so.2",
    NULL,
};
#else
#error "libtgcompat-exec supports only 64-bit AArch64 and x86-64 hosts"
#endif

typedef int (*execve_function)(const char *, char *const[], char *const[]);
typedef int (*execvpe_function)(const char *, char *const[], char *const[]);
typedef int (*posix_spawn_function)(pid_t *, const char *,
    const posix_spawn_file_actions_t *, const posix_spawnattr_t *,
    char *const[], char *const[]);

static execve_function real_execve;
static execvpe_function real_execvpe;
static posix_spawn_function real_posix_spawn;
static posix_spawn_function real_posix_spawnp;

extern char **environ;

enum wrap_result {
    WRAP_ERROR = -1,
    WRAP_NO = 0,
    WRAP_YES = 1,
};

struct loader_invocation {
    char **arguments;
    char *filename;
};

static char *environment_value(char *const envp[], const char *name) {
    size_t index;
    size_t name_length;

    if (envp == NULL) {
        return NULL;
    }
    name_length = strlen(name);
    for (index = 0; envp[index] != NULL; ++index) {
        if (strncmp(envp[index], name, name_length) == 0 &&
                envp[index][name_length] == '=') {
            return envp[index] + name_length + 1U;
        }
    }
    return NULL;
}

static bool disabled_by_environment(char *const envp[]) {
    char *value = environment_value(envp, "TGCOMPAT_EXEC_DISABLE");

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static bool interpreter_matches(const char *interpreter,
        char *const envp[]) {
    char *override = environment_value(
        envp, "TGCOMPAT_EXEC_MATCH_INTERPRETER");
    size_t index;

    if (override != NULL && override[0] != '\0') {
        return strcmp(interpreter, override) == 0;
    }
    for (index = 0; default_interpreters[index] != NULL; ++index) {
        if (strcmp(interpreter, default_interpreters[index]) == 0) {
            return true;
        }
    }
    return false;
}

static bool read_elf_interpreter(const char *filename, char *interpreter,
    size_t capacity);

static bool should_wrap(const char *filename, char *const envp[]) {
    char interpreter[PATH_MAX];
    char *loader = environment_value(envp, "TGCOMPAT_LD_SO");

    return loader != NULL && loader[0] == '/' &&
        !disabled_by_environment(envp) &&
        read_elf_interpreter(filename, interpreter, sizeof(interpreter)) &&
        interpreter_matches(interpreter, envp);
}

static bool read_exact_at(int descriptor, void *buffer, size_t length,
        off_t offset) {
    unsigned char *cursor = buffer;
    size_t consumed = 0;

    if (offset < 0 || length > (size_t)INT64_MAX ||
            (uint64_t)offset > (uint64_t)INT64_MAX - length) {
        return false;
    }
    while (consumed < length) {
        ssize_t result = pread(descriptor, cursor + consumed,
            length - consumed, offset + (off_t)consumed);

        if (result > 0) {
            consumed += (size_t)result;
            continue;
        }
        if (result < 0 && errno == EINTR) {
            continue;
        }
        return false;
    }
    return true;
}

static bool read_elf_interpreter(const char *filename, char *interpreter,
        size_t capacity) {
    Elf64_Ehdr header;
    struct stat metadata;
    int descriptor;
    size_t index;
    bool found = false;

    descriptor = open(filename, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return false;
    }
    if (fstat(descriptor, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            !read_exact_at(descriptor, &header, sizeof(header), 0) ||
            memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
            header.e_ident[EI_CLASS] != ELFCLASS64 ||
            header.e_ident[EI_DATA] != ELFDATA2LSB ||
            header.e_machine != TGCOMPAT_ELF_MACHINE ||
            header.e_phentsize != sizeof(Elf64_Phdr) ||
            header.e_phnum == 0 || header.e_phnum > 128) {
        (void)close(descriptor);
        return false;
    }
    if (header.e_phoff > (Elf64_Off)INT64_MAX ||
            (uint64_t)header.e_phnum >
                ((uint64_t)INT64_MAX - header.e_phoff) /
                    sizeof(Elf64_Phdr) ||
            header.e_phoff +
                (uint64_t)header.e_phnum * sizeof(Elf64_Phdr) >
                    (uint64_t)metadata.st_size) {
        (void)close(descriptor);
        return false;
    }

    for (index = 0; index < header.e_phnum; ++index) {
        Elf64_Phdr program_header;
        off_t offset = (off_t)header.e_phoff +
            (off_t)(index * sizeof(program_header));

        if (!read_exact_at(descriptor, &program_header,
                sizeof(program_header), offset)) {
            break;
        }
        if (program_header.p_type != PT_INTERP) {
            continue;
        }
        if (program_header.p_filesz < 2 ||
                program_header.p_filesz > capacity ||
                program_header.p_offset > (Elf64_Off)INT64_MAX ||
                program_header.p_offset + program_header.p_filesz >
                    (uint64_t)metadata.st_size ||
                !read_exact_at(descriptor, interpreter,
                    (size_t)program_header.p_filesz,
                    (off_t)program_header.p_offset) ||
                interpreter[program_header.p_filesz - 1U] != '\0') {
            break;
        }
        found = true;
        break;
    }
    (void)close(descriptor);
    return found;
}

static execve_function resolve_execve(void) {
    void *symbol;
    execve_function function = NULL;

    symbol = dlsym(RTLD_NEXT, "execve");
    _Static_assert(sizeof(function) == sizeof(symbol),
        "function and data pointers must have equal size");
    memcpy(&function, &symbol, sizeof(function));
    return function;
}

static execvpe_function resolve_execvpe(void) {
    void *symbol;
    execvpe_function function = NULL;

    symbol = dlsym(RTLD_NEXT, "execvpe");
    _Static_assert(sizeof(function) == sizeof(symbol),
        "function and data pointers must have equal size");
    memcpy(&function, &symbol, sizeof(function));
    return function;
}

static posix_spawn_function resolve_posix_spawn(const char *name) {
    void *symbol;
    posix_spawn_function function = NULL;

    symbol = dlsym(RTLD_NEXT, name);
    _Static_assert(sizeof(function) == sizeof(symbol),
        "function and data pointers must have equal size");
    memcpy(&function, &symbol, sizeof(function));
    return function;
}

__attribute__((constructor)) static void initialize_exec_shim(void) {
    real_execve = resolve_execve();
    real_execvpe = resolve_execvpe();
    real_posix_spawn = resolve_posix_spawn("posix_spawn");
    real_posix_spawnp = resolve_posix_spawn("posix_spawnp");
}

static enum wrap_result build_loader_arguments(const char *filename,
        char *const argv[], char *const envp[],
        struct loader_invocation *invocation) {
    char *loader;
    char *library_path;
    char *filename_copy;
    char **loader_argv;
    size_t argument_count = 0;
    size_t output_index = 0;
    size_t input_index;

    loader = environment_value(envp, "TGCOMPAT_LD_SO");
    if (!should_wrap(filename, envp)) {
        return WRAP_NO;
    }

    while (argv[argument_count] != NULL) {
        if (argument_count == SIZE_MAX - 8U) {
            errno = E2BIG;
            return WRAP_ERROR;
        }
        ++argument_count;
    }

    library_path = environment_value(envp, "TGCOMPAT_LIBRARY_PATH");
    filename_copy = strdup(filename);
    if (filename_copy == NULL) {
        return WRAP_ERROR;
    }
    loader_argv = calloc(argument_count + 8U, sizeof(*loader_argv));
    if (loader_argv == NULL) {
        free(filename_copy);
        return WRAP_ERROR;
    }

    loader_argv[output_index++] = loader;
    loader_argv[output_index++] = "--inhibit-cache";
    loader_argv[output_index++] = "--argv0";
    loader_argv[output_index++] =
        argv[0] != NULL ? argv[0] : filename_copy;
    if (library_path != NULL && library_path[0] != '\0') {
        loader_argv[output_index++] = "--library-path";
        loader_argv[output_index++] = library_path;
    }
    loader_argv[output_index++] = filename_copy;
    for (input_index = 1; input_index < argument_count; ++input_index) {
        loader_argv[output_index++] = argv[input_index];
    }
    loader_argv[output_index] = NULL;

    invocation->arguments = loader_argv;
    invocation->filename = filename_copy;
    return WRAP_YES;
}

static void free_loader_arguments(struct loader_invocation *invocation) {
    if (invocation->arguments == NULL) {
        return;
    }
    free(invocation->filename);
    free(invocation->arguments);
    invocation->filename = NULL;
    invocation->arguments = NULL;
}

__attribute__((visibility("default"))) int execve(const char *filename,
        char *const argv[], char *const envp[]) {
    struct loader_invocation invocation = { 0 };
    enum wrap_result wrap;
    int result;
    int saved_errno;

    if (real_execve == NULL) {
        real_execve = resolve_execve();
        if (real_execve == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }

    wrap = build_loader_arguments(filename, argv, envp, &invocation);
    if (wrap == WRAP_NO) {
        return real_execve(filename, argv, envp);
    }
    if (wrap == WRAP_ERROR) {
        return -1;
    }

    result = real_execve(invocation.arguments[0], invocation.arguments, envp);
    saved_errno = errno;
    free_loader_arguments(&invocation);
    errno = saved_errno;
    return result;
}

static char *find_matching_path(const char *file, char *const envp[]) {
    const char *path;
    const char *cursor;

    if (strchr(file, '/') != NULL) {
        return should_wrap(file, envp) ? strdup(file) : NULL;
    }
    path = environment_value(environ, "PATH");
    if (path == NULL) {
        path = "/bin:/usr/bin";
    }
    cursor = path;
    for (;;) {
        const char *separator = strchr(cursor, ':');
        size_t directory_length = separator != NULL
            ? (size_t)(separator - cursor) : strlen(cursor);
        size_t file_length = strlen(file);
        size_t capacity;
        char *candidate;

        if (directory_length > SIZE_MAX - file_length - 2U) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        capacity = directory_length + file_length + 2U;
        candidate = malloc(capacity);
        if (candidate == NULL) {
            return NULL;
        }
        if (directory_length == 0) {
            memcpy(candidate, file, file_length + 1U);
        } else {
            memcpy(candidate, cursor, directory_length);
            candidate[directory_length] = '/';
            memcpy(candidate + directory_length + 1U, file,
                file_length + 1U);
        }
        if (access(candidate, X_OK) == 0 && should_wrap(candidate, envp)) {
            return candidate;
        }
        free(candidate);
        if (separator == NULL) {
            break;
        }
        cursor = separator + 1;
    }
    return NULL;
}

__attribute__((visibility("default"))) int execv(const char *path,
        char *const argv[]) {
    return execve(path, argv, environ);
}

__attribute__((visibility("default"))) int execvpe(const char *file,
        char *const argv[], char *const envp[]) {
    char *candidate;
    int result;
    int saved_errno;

    if (strchr(file, '/') != NULL) {
        return execve(file, argv, envp);
    }
    candidate = find_matching_path(file, envp);
    if (candidate != NULL) {
        result = execve(candidate, argv, envp);
        saved_errno = errno;
        free(candidate);
        errno = saved_errno;
        return result;
    }
    if (real_execvpe == NULL) {
        real_execvpe = resolve_execvpe();
        if (real_execvpe == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }
    return real_execvpe(file, argv, envp);
}

__attribute__((visibility("default"))) int execvp(const char *file,
        char *const argv[]) {
    return execvpe(file, argv, environ);
}

static char **collect_variadic_arguments(const char *first, va_list source,
        char *const **environment_out) {
    va_list scan;
    const char *value;
    size_t count = 0;
    size_t index;
    char **arguments;

    va_copy(scan, source);
    value = first;
    while (value != NULL) {
        if (count == SIZE_MAX - 1U) {
            va_end(scan);
            errno = E2BIG;
            return NULL;
        }
        ++count;
        value = va_arg(scan, const char *);
    }
    if (environment_out != NULL) {
        *environment_out = va_arg(scan, char *const *);
    }
    va_end(scan);

    arguments = calloc(count + 1U, sizeof(*arguments));
    if (arguments == NULL) {
        return NULL;
    }
    value = first;
    for (index = 0; index < count; ++index) {
        _Static_assert(sizeof(arguments[index]) == sizeof(value),
            "const and mutable character pointers must have equal size");
        memcpy(&arguments[index], &value, sizeof(value));
        value = va_arg(source, const char *);
    }
    arguments[count] = NULL;
    return arguments;
}

__attribute__((visibility("default"))) int execl(const char *path,
        const char *arg, ...) {
    va_list arguments_source;
    char **arguments;
    int result;
    int saved_errno;

    va_start(arguments_source, arg);
    arguments = collect_variadic_arguments(arg, arguments_source, NULL);
    va_end(arguments_source);
    if (arguments == NULL) {
        return -1;
    }
    result = execve(path, arguments, environ);
    saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int execle(const char *path,
        const char *arg, ...) {
    va_list arguments_source;
    char *const *environment = NULL;
    char **arguments;
    int result;
    int saved_errno;

    va_start(arguments_source, arg);
    arguments = collect_variadic_arguments(arg, arguments_source,
        &environment);
    va_end(arguments_source);
    if (arguments == NULL) {
        return -1;
    }
    result = execve(path, arguments, environment);
    saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}

__attribute__((visibility("default"))) int execlp(const char *file,
        const char *arg, ...) {
    va_list arguments_source;
    char **arguments;
    int result;
    int saved_errno;

    va_start(arguments_source, arg);
    arguments = collect_variadic_arguments(arg, arguments_source, NULL);
    va_end(arguments_source);
    if (arguments == NULL) {
        return -1;
    }
    result = execvpe(file, arguments, environ);
    saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}

static int spawn_wrapped(posix_spawn_function function, pid_t *pid,
        const char *path, const posix_spawn_file_actions_t *file_actions,
        const posix_spawnattr_t *attributes, char *const argv[],
        char *const envp[]) {
    struct loader_invocation invocation = { 0 };
    enum wrap_result wrap = build_loader_arguments(path, argv, envp,
        &invocation);
    int result;

    if (wrap == WRAP_ERROR) {
        return errno != 0 ? errno : ENOMEM;
    }
    if (wrap == WRAP_NO) {
        return function(pid, path, file_actions, attributes, argv, envp);
    }
    result = function(pid, invocation.arguments[0], file_actions, attributes,
        invocation.arguments, envp);
    free_loader_arguments(&invocation);
    return result;
}

__attribute__((visibility("default"))) int posix_spawn(pid_t *pid,
        const char *path, const posix_spawn_file_actions_t *file_actions,
        const posix_spawnattr_t *attributes, char *const argv[],
        char *const envp[]) {
    if (real_posix_spawn == NULL) {
        real_posix_spawn = resolve_posix_spawn("posix_spawn");
        if (real_posix_spawn == NULL) {
            return ENOSYS;
        }
    }
    return spawn_wrapped(real_posix_spawn, pid, path, file_actions,
        attributes, argv, envp);
}

__attribute__((visibility("default"))) int posix_spawnp(pid_t *pid,
        const char *file, const posix_spawn_file_actions_t *file_actions,
        const posix_spawnattr_t *attributes, char *const argv[],
        char *const envp[]) {
    char *candidate;
    int result;

    if (real_posix_spawnp == NULL) {
        real_posix_spawnp = resolve_posix_spawn("posix_spawnp");
        if (real_posix_spawnp == NULL) {
            return ENOSYS;
        }
    }
    if (strchr(file, '/') != NULL) {
        if (real_posix_spawn == NULL) {
            real_posix_spawn = resolve_posix_spawn("posix_spawn");
            if (real_posix_spawn == NULL) {
                return ENOSYS;
            }
        }
        return spawn_wrapped(real_posix_spawn, pid, file, file_actions,
            attributes, argv, envp);
    }
    candidate = find_matching_path(file, envp);
    if (candidate == NULL) {
        return real_posix_spawnp(pid, file, file_actions, attributes, argv,
            envp);
    }
    if (real_posix_spawn == NULL) {
        real_posix_spawn = resolve_posix_spawn("posix_spawn");
        if (real_posix_spawn == NULL) {
            free(candidate);
            return ENOSYS;
        }
    }
    result = spawn_wrapped(real_posix_spawn, pid, candidate, file_actions,
        attributes, argv, envp);
    free(candidate);
    return result;
}
