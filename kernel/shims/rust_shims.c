#include <stddef.h>
#include <stdint.h>

// Bulk copies go word-at-a-time via rep movs/stos on the native word size.
// QEMU TCG emulates string ops one iteration at a time, so iteration count
// is what matters: byte loops capped compositor restores and framebuffer
// presents at ~20MB/s. Word-sized iterations cut that by 8x (4x on i686),
// and real hardware is happy with either.

#if defined(__x86_64__)
#define WORD_T uint64_t
#define REP_MOVS_WORD "rep movsq"
#define REP_STOS_WORD "rep stosq"
#else
#define WORD_T uint32_t
#define REP_MOVS_WORD "rep movsl"
#define REP_STOS_WORD "rep stosl"
#endif

void *memcpy(void *dest, const void *src, size_t n) {
    void *d = dest;
    size_t words = n / sizeof(WORD_T);
    size_t tail = n % sizeof(WORD_T);
    __asm__ volatile(REP_MOVS_WORD
                     : "+D"(d), "+S"(src), "+c"(words)
                     :
                     : "memory");
    __asm__ volatile("rep movsb"
                     : "+D"(d), "+S"(src), "+c"(tail)
                     :
                     : "memory");
    return dest;
}

void *memset(void *s, int c, size_t n) {
    void *p = s;
    WORD_T pattern = (uint8_t)c;
    pattern |= pattern << 8;
    pattern |= pattern << 16;
#if defined(__x86_64__)
    pattern |= pattern << 32;
#endif
    size_t words = n / sizeof(WORD_T);
    size_t tail = n % sizeof(WORD_T);
    __asm__ volatile(REP_STOS_WORD
                     : "+D"(p), "+c"(words)
                     : "a"(pattern)
                     : "memory");
    __asm__ volatile("rep stosb"
                     : "+D"(p), "+c"(tail)
                     : "a"((uint8_t)c)
                     : "memory");
    return s;
}

void *memmove(void *dest, const void *src, size_t n) {
    if (dest == src || n == 0) {
        return dest;
    }
    if ((uintptr_t)dest < (uintptr_t)src) {
        return memcpy(dest, src, n);
    }
    // Overlapping with dest above src: copy backwards
    void *d = (uint8_t *)dest + n - 1;
    const void *sp = (const uint8_t *)src + n - 1;
    __asm__ volatile("std\n\trep movsb\n\tcld"
                     : "+D"(d), "+S"(sp), "+c"(n)
                     :
                     : "memory");
    return dest;
}

int memcmp(const void *s1, const void *s2, size_t n) {
    const uint8_t *a = s1, *b = s2;
    for (size_t i = 0; i < n; i++) {
        if (a[i] != b[i]) return a[i] - b[i];
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t len = 0;
    while (s[len]) len++;
    return len;
}
