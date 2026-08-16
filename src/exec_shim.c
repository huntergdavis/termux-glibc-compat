#define _GNU_SOURCE

#include <dlfcn.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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

static execve_function real_execve;

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

__attribute__((constructor)) static void initialize_exec_shim(void) {
    real_execve = resolve_execve();
}

__attribute__((visibility("default"))) int execve(const char *filename,
        char *const argv[], char *const envp[]) {
    char interpreter[PATH_MAX];
    char *loader;
    char *library_path;
    char *filename_copy;
    char **loader_argv;
    size_t argument_count = 0;
    size_t output_index = 0;
    size_t input_index;
    int result;
    int saved_errno;

    if (real_execve == NULL) {
        real_execve = resolve_execve();
        if (real_execve == NULL) {
            errno = ENOSYS;
            return -1;
        }
    }

    loader = environment_value(envp, "TGCOMPAT_LD_SO");
    if (loader == NULL || loader[0] != '/' || disabled_by_environment(envp) ||
            !read_elf_interpreter(filename, interpreter,
                sizeof(interpreter)) ||
            !interpreter_matches(interpreter, envp)) {
        return real_execve(filename, argv, envp);
    }

    while (argv[argument_count] != NULL) {
        if (argument_count == SIZE_MAX - 8U) {
            errno = E2BIG;
            return -1;
        }
        ++argument_count;
    }

    library_path = environment_value(envp, "TGCOMPAT_LIBRARY_PATH");
    filename_copy = strdup(filename);
    if (filename_copy == NULL) {
        return -1;
    }
    loader_argv = calloc(argument_count + 8U, sizeof(*loader_argv));
    if (loader_argv == NULL) {
        free(filename_copy);
        return -1;
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

    result = real_execve(loader, loader_argv, envp);
    saved_errno = errno;
    free(loader_argv);
    free(filename_copy);
    errno = saved_errno;
    return result;
}
