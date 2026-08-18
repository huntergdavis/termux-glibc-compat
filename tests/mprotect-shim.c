#define _GNU_SOURCE

#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <unistd.h>

int main(void) {
    long page_size = sysconf(_SC_PAGESIZE);
    char template[] = "/tmp/tgcompat-mprotect.XXXXXX";
    unsigned char *mapping;
    int descriptor;
    size_t index;

    assert(page_size > 0);
    descriptor = mkstemp(template);
    assert(descriptor >= 0);
    assert(unlink(template) == 0);
    assert(ftruncate(descriptor, page_size) == 0);
    mapping = mmap(NULL, (size_t)page_size, PROT_READ | PROT_WRITE,
        MAP_PRIVATE, descriptor, 0);
    assert(mapping != MAP_FAILED);
    assert(close(descriptor) == 0);

    for (index = 0; index < (size_t)page_size; ++index) {
        mapping[index] = (unsigned char)(index * 37U + 11U);
    }
    assert(mprotect(mapping, (size_t)page_size,
        PROT_READ | PROT_EXEC) == 0);
    for (index = 0; index < (size_t)page_size; ++index) {
        assert(mapping[index] == (unsigned char)(index * 37U + 11U));
    }
    assert(mprotect(mapping, (size_t)page_size,
        PROT_READ | PROT_WRITE) == 0);
    mapping[page_size - 1] = 0x5a;
    assert(mapping[page_size - 1] == 0x5a);
    assert(munmap(mapping, (size_t)page_size) == 0);

    puts("noexec mprotect anonymous-copy shim: PASS");
    return 0;
}
