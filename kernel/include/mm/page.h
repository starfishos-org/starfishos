#include <common/types.h>
#include <arch/mm/page_table.h>

static inline void pagecpy_nt(void *dst, const void *src)
{
    extern void memcpy_nt(void *dst, void *src, size_t len);
    memcpy_nt(dst, (void *)src, PAGE_SIZE);
}

static inline void pagecpy(void *dst, const void *src)
{
#if 0
    return memcpy(dst, src, PAGE_SIZE);
#else
    unsigned char *d = dst;
    const unsigned char *s = src;
    size_t n = PAGE_SIZE;

    for (; n >= 64; s += 64, d += 64, n -= 64) {
        *(u64 *)(d + 0) = *(u64 *)(s + 0);
        *(u64 *)(d + 8) = *(u64 *)(s + 8);
        *(u64 *)(d + 16) = *(u64 *)(s + 16);
        *(u64 *)(d + 24) = *(u64 *)(s + 24);
        *(u64 *)(d + 32) = *(u64 *)(s + 32);
        *(u64 *)(d + 40) = *(u64 *)(s + 40);
        *(u64 *)(d + 48) = *(u64 *)(s + 48);
        *(u64 *)(d + 56) = *(u64 *)(s + 56);
    }
#endif
}
