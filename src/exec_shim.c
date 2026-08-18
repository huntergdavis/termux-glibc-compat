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
    char **environment;
    char *ld_preload_assignment;
    char *proc_self_exe_assignment;
};

struct environment_override {
    char **values;
    char *ld_preload_assignment;
};

static const char ld_preload_name[] = "LD_PRELOAD";
static const char proc_self_exe_name[] = "TGCOMPAT_PROC_SELF_EXE";
static const char shell_redirect_name[] = "TGCOMPAT_EXEC_SHELL";
static const char path_from_name[] = "TGCOMPAT_EXEC_PATH_FROM";
static const char path_to_name[] = "TGCOMPAT_EXEC_PATH_TO";
static const char final_path_prefix_name[] =
    "TGCOMPAT_EXEC_FINAL_PATH_PREFIX";
static const char final_ld_preload_name[] =
    "TGCOMPAT_EXEC_FINAL_LD_PRELOAD";

static bool interpreter_matches(const char *interpreter,
    char *const envp[]);

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

static char *configured_environment_value(char *const envp[],
        const char *name) {
    char *value = environment_value(envp, name);

    return value != NULL ? value : environment_value(environ, name);
}

static bool final_path_matches(const char *filename, char *const envp[]) {
    char *prefix = configured_environment_value(
        envp, final_path_prefix_name);
    size_t prefix_length;

    if (filename == NULL || filename[0] != '/' || prefix == NULL ||
            prefix[0] != '/') {
        return false;
    }
    prefix_length = strlen(prefix);
    return prefix_length > 1U && prefix[prefix_length - 1U] == '/' &&
        strncmp(filename, prefix, prefix_length) == 0;
}

static int build_final_environment(const char *filename,
        char *const envp[], struct environment_override *override) {
    char *preload;
    char *assignment;
    char **values;
    size_t preload_length;
    size_t environment_count = 0;
    size_t keep_count = 0;
    size_t index;
    size_t output = 0;
    size_t name_length = sizeof(ld_preload_name) - 1U;

    if (!final_path_matches(filename, envp)) {
        return 0;
    }
    preload = configured_environment_value(envp, final_ld_preload_name);
    if (preload == NULL || envp == NULL) {
        errno = EINVAL;
        return -1;
    }
    while (envp[environment_count] != NULL) {
        if (strncmp(envp[environment_count], ld_preload_name,
                name_length) != 0 ||
                envp[environment_count][name_length] != '=') {
            ++keep_count;
        }
        if (environment_count == SIZE_MAX - 2U) {
            errno = E2BIG;
            return -1;
        }
        ++environment_count;
    }
    preload_length = strlen(preload);
    if (preload_length > SIZE_MAX - name_length - 2U) {
        errno = E2BIG;
        return -1;
    }
    assignment = malloc(name_length + preload_length + 2U);
    values = calloc(keep_count + 2U, sizeof(*values));
    if (assignment == NULL || values == NULL) {
        free(values);
        free(assignment);
        return -1;
    }
    memcpy(assignment, ld_preload_name, name_length);
    assignment[name_length] = '=';
    memcpy(assignment + name_length + 1U, preload, preload_length + 1U);
    for (index = 0; index < environment_count; ++index) {
        if (strncmp(envp[index], ld_preload_name, name_length) == 0 &&
                envp[index][name_length] == '=') {
            continue;
        }
        values[output++] = envp[index];
    }
    values[output++] = assignment;
    values[output] = NULL;
    override->values = values;
    override->ld_preload_assignment = assignment;
    return 1;
}

static void free_environment_override(struct environment_override *override) {
    free(override->ld_preload_assignment);
    free(override->values);
    override->ld_preload_assignment = NULL;
    override->values = NULL;
}

static bool disabled_by_environment(char *const envp[]) {
    char *value = environment_value(envp, "TGCOMPAT_EXEC_DISABLE");

    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static const char *redirect_shell_path(const char *filename,
        char *const envp[]) {
    char *redirect;

    if (filename == NULL ||
            (strcmp(filename, "/bin/sh") != 0 &&
                strcmp(filename, "/usr/bin/sh") != 0)) {
        return filename;
    }
    redirect = environment_value(envp, shell_redirect_name);
    if (redirect == NULL) {
        redirect = environment_value(environ, shell_redirect_name);
    }
    if (redirect == NULL || redirect[0] != '/' || redirect[1] == '\0') {
        return filename;
    }
    return redirect;
}

static const char *redirect_configured_path(const char *filename,
        char *const envp[]) {
    char *from;
    char *to;

    if (filename == NULL) {
        return NULL;
    }
    from = environment_value(envp, path_from_name);
    to = environment_value(envp, path_to_name);
    if (from == NULL && to == NULL) {
        from = environment_value(environ, path_from_name);
        to = environment_value(environ, path_to_name);
    }
    if (from == NULL || to == NULL || from[0] != '/' || from[1] == '\0' ||
            to[0] != '/' || to[1] == '\0' || strcmp(filename, from) != 0) {
        return filename;
    }
    return to;
}

static const char *redirect_exec_path(const char *filename,
        char *const envp[]) {
    const char *redirect = redirect_configured_path(filename, envp);
    char *loader;

    if (redirect != filename) {
        return redirect;
    }
    redirect = redirect_shell_path(filename, envp);
    if (redirect != filename || filename == NULL ||
            disabled_by_environment(envp) ||
            !interpreter_matches(filename, envp)) {
        return redirect;
    }
    loader = environment_value(envp, "TGCOMPAT_LD_SO");
    if (loader == NULL || loader[0] != '/' || loader[1] == '\0') {
        return filename;
    }
    return loader;
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
    const char *launch_filename;
    char *loader;
    char *library_path;
    char *ld_preload_override;
    char *filename_copy;
    char **loader_argv;
    size_t argument_count = 0;
    size_t environment_count = 0;
    size_t environment_keep_count = 0;
    size_t output_index = 0;
    size_t input_index;
    size_t name_length = sizeof(proc_self_exe_name) - 1U;
    size_t ld_preload_name_length = sizeof(ld_preload_name) - 1U;
    size_t ld_preload_override_length;
    size_t original_path_length;
    char *original_path;
    char *assignment;
    char *ld_preload_assignment = NULL;
    char **loader_environment;

    launch_filename = redirect_exec_path(filename, envp);
    loader = environment_value(envp, "TGCOMPAT_LD_SO");
    if (!should_wrap(launch_filename, envp)) {
        return WRAP_NO;
    }

    while (argv[argument_count] != NULL) {
        if (argument_count == SIZE_MAX - 8U) {
            errno = E2BIG;
            return WRAP_ERROR;
        }
        ++argument_count;
    }
    ld_preload_override = environment_value(
        envp, "TGCOMPAT_EXEC_LD_PRELOAD");
    if (ld_preload_override == NULL) {
        ld_preload_override = environment_value(
            environ, "TGCOMPAT_EXEC_LD_PRELOAD");
    }
    if (envp != NULL) {
        while (envp[environment_count] != NULL) {
            if (environment_count == SIZE_MAX - 3U) {
                errno = E2BIG;
                return WRAP_ERROR;
            }
            if (strncmp(envp[environment_count], proc_self_exe_name,
                    name_length) != 0 ||
                    envp[environment_count][name_length] != '=') {
                if (ld_preload_override != NULL &&
                        strncmp(envp[environment_count], ld_preload_name,
                            ld_preload_name_length) == 0 &&
                        envp[environment_count][ld_preload_name_length] == '=') {
                    ++environment_count;
                    continue;
                }
                ++environment_keep_count;
            }
            ++environment_count;
        }
    }

    library_path = environment_value(envp, "TGCOMPAT_LIBRARY_PATH");
    filename_copy = strdup(launch_filename);
    if (filename_copy == NULL) {
        return WRAP_ERROR;
    }
    loader_argv = calloc(argument_count + 8U, sizeof(*loader_argv));
    if (loader_argv == NULL) {
        free(filename_copy);
        return WRAP_ERROR;
    }
    original_path = realpath(launch_filename, NULL);
    if (original_path == NULL) {
        free(filename_copy);
        free(loader_argv);
        return WRAP_ERROR;
    }
    original_path_length = strlen(original_path);
    if (original_path_length > SIZE_MAX - name_length - 2U) {
        free(original_path);
        free(filename_copy);
        free(loader_argv);
        errno = ENAMETOOLONG;
        return WRAP_ERROR;
    }
    assignment = malloc(name_length + original_path_length + 2U);
    loader_environment = calloc(environment_keep_count +
        (ld_preload_override != NULL ? 3U : 2U),
        sizeof(*loader_environment));
    if (assignment == NULL || loader_environment == NULL) {
        free(loader_environment);
        free(assignment);
        free(original_path);
        free(filename_copy);
        free(loader_argv);
        return WRAP_ERROR;
    }
    if (ld_preload_override != NULL) {
        ld_preload_override_length = strlen(ld_preload_override);
        if (ld_preload_override_length > SIZE_MAX -
                ld_preload_name_length - 2U) {
            free(loader_environment);
            free(assignment);
            free(filename_copy);
            free(loader_argv);
            errno = ENAMETOOLONG;
            return WRAP_ERROR;
        }
        ld_preload_assignment = malloc(ld_preload_name_length +
            ld_preload_override_length + 2U);
        if (ld_preload_assignment == NULL) {
            free(loader_environment);
            free(assignment);
            free(filename_copy);
            free(loader_argv);
            return WRAP_ERROR;
        }
        memcpy(ld_preload_assignment, ld_preload_name,
            ld_preload_name_length);
        ld_preload_assignment[ld_preload_name_length] = '=';
        memcpy(ld_preload_assignment + ld_preload_name_length + 1U,
            ld_preload_override, ld_preload_override_length + 1U);
    }
    memcpy(assignment, proc_self_exe_name, name_length);
    assignment[name_length] = '=';
    memcpy(assignment + name_length + 1U, original_path,
        original_path_length + 1U);
    free(original_path);

    output_index = 0;
    for (input_index = 0; input_index < environment_count; ++input_index) {
        if (strncmp(envp[input_index], proc_self_exe_name, name_length) == 0 &&
                envp[input_index][name_length] == '=') {
            continue;
        }
        if (ld_preload_override != NULL &&
                strncmp(envp[input_index], ld_preload_name,
                    ld_preload_name_length) == 0 &&
                envp[input_index][ld_preload_name_length] == '=') {
            continue;
        }
        loader_environment[output_index++] = envp[input_index];
    }
    if (ld_preload_assignment != NULL) {
        loader_environment[output_index++] = ld_preload_assignment;
    }
    loader_environment[output_index++] = assignment;
    loader_environment[output_index] = NULL;

    output_index = 0;
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
    invocation->environment = loader_environment;
    invocation->ld_preload_assignment = ld_preload_assignment;
    invocation->proc_self_exe_assignment = assignment;
    return WRAP_YES;
}

static void free_loader_arguments(struct loader_invocation *invocation) {
    if (invocation->arguments == NULL) {
        return;
    }
    free(invocation->filename);
    free(invocation->arguments);
    free(invocation->ld_preload_assignment);
    free(invocation->proc_self_exe_assignment);
    free(invocation->environment);
    invocation->filename = NULL;
    invocation->arguments = NULL;
    invocation->ld_preload_assignment = NULL;
    invocation->proc_self_exe_assignment = NULL;
    invocation->environment = NULL;
}

__attribute__((visibility("default"))) int execve(const char *filename,
        char *const argv[], char *const envp[]) {
    struct loader_invocation invocation = { 0 };
    struct environment_override final_environment = { 0 };
    enum wrap_result wrap;
    int result;
    int final_result;
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
        final_result = build_final_environment(
            filename, envp, &final_environment);
        if (final_result < 0) {
            return -1;
        }
        result = real_execve(
            redirect_exec_path(filename, envp), argv,
            final_result > 0 ? final_environment.values : envp);
        saved_errno = errno;
        free_environment_override(&final_environment);
        errno = saved_errno;
        return result;
    }
    if (wrap == WRAP_ERROR) {
        return -1;
    }

    result = real_execve(invocation.arguments[0], invocation.arguments,
        invocation.environment);
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
    struct environment_override final_environment = { 0 };
    enum wrap_result wrap = build_loader_arguments(path, argv, envp,
        &invocation);
    int result;
    int final_result;

    if (wrap == WRAP_ERROR) {
        return errno != 0 ? errno : ENOMEM;
    }
    if (wrap == WRAP_NO) {
        final_result = build_final_environment(
            path, envp, &final_environment);
        if (final_result < 0) {
            return errno != 0 ? errno : ENOMEM;
        }
        result = function(pid, redirect_exec_path(path, envp), file_actions,
            attributes, argv,
            final_result > 0 ? final_environment.values : envp);
        free_environment_override(&final_environment);
        return result;
    }
    result = function(pid, invocation.arguments[0], file_actions, attributes,
        invocation.arguments, invocation.environment);
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
