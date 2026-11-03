#include <stddef.h>
#include <stdint.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <sys/mman.h>
#endif

void *jit_alloc_exec(size_t size, const void *hint);

// Memory that can hold generated code, placed close to hint when possible so that code can call
// the emulator's functions with 32-bit displacements.
void *jit_alloc_exec(size_t size, const void *hint)
{
    const uintptr_t step = (uintptr_t)1 << 26;
    uintptr_t base = (uintptr_t)hint & ~(step - 1);
    for (uintptr_t d = step; d < ((uintptr_t)1 << 30); d += step) {
        for (int sign = -1; sign <= 1; sign += 2) {
            uintptr_t at = sign < 0 ? base - d - size : base + d;
            if (sign < 0 && base < d + size)
                continue;
#ifdef _WIN32
            void *p = VirtualAlloc((void *)at, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
            if (p)
                return p;
#else
            void *p = mmap((void *)at, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            if (p == MAP_FAILED)
                return NULL;
            uintptr_t got = (uintptr_t)p, mid = (uintptr_t)hint;
            if ((got > mid ? got + size - mid : mid - got) < ((uintptr_t)1 << 31))
                return p;
            munmap(p, size);
#endif
        }
    }
#ifdef _WIN32
    return VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
#else
    void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
#endif
}
