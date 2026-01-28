#include <common/types.h>

void memcpy(void *dst, const void *src, size_t size)
{
    char *d = (char *)dst;
    const char *s = (const char *)src;
    size_t i;

    /* Optimize for large copies (>= 64 bytes): use 64-byte blocks */
    if (size >= 64) {
        size_t n64 = size / 64;
        u64 *d64 = (u64 *)d;
        const u64 *s64 = (const u64 *)s;
        
        /* Copy 64 bytes at a time (8 x 64-bit registers) */
        for (i = 0; i < n64; i++) {
            /* Unroll loop: copy 8 x 8 bytes = 64 bytes */
            d64[i * 8 + 0] = s64[i * 8 + 0];
            d64[i * 8 + 1] = s64[i * 8 + 1];
            d64[i * 8 + 2] = s64[i * 8 + 2];
            d64[i * 8 + 3] = s64[i * 8 + 3];
            d64[i * 8 + 4] = s64[i * 8 + 4];
            d64[i * 8 + 5] = s64[i * 8 + 5];
            d64[i * 8 + 6] = s64[i * 8 + 6];
            d64[i * 8 + 7] = s64[i * 8 + 7];
        }
        
        /* Handle remaining bytes */
        size_t remain = size % 64;
        if (remain > 0) {
            char *d_tail = d + n64 * 64;
            const char *s_tail = s + n64 * 64;
            /* Use 64-bit for remaining if >= 8 bytes */
            size_t remain64 = remain / 8;
            u64 *d_tail64 = (u64 *)d_tail;
            const u64 *s_tail64 = (const u64 *)s_tail;
            for (i = 0; i < remain64; i++) {
                d_tail64[i] = s_tail64[i];
            }
            /* Handle final bytes (< 8) */
            size_t final_bytes = remain % 8;
            if (final_bytes > 0) {
                char *d_final = d_tail + remain64 * 8;
                const char *s_final = s_tail + remain64 * 8;
                for (i = 0; i < final_bytes; i++) {
                    d_final[i] = s_final[i];
                }
            }
        }
    } else {
        /* Small copies: use 64-bit when possible */
        size_t n64 = size / 8;
        u64 *d64 = (u64 *)d;
        const u64 *s64 = (const u64 *)s;
        
        for (i = 0; i < n64; i++) {
            d64[i] = s64[i];
        }
        
        /* Handle remaining bytes */
        size_t remain = size % 8;
        if (remain > 0) {
            char *d_tail = d + n64 * 8;
            const char *s_tail = s + n64 * 8;
            for (i = 0; i < remain; i++) {
                d_tail[i] = s_tail[i];
            }
        }
    }
}

void memset(void *dst, const char ch, size_t size)
{
    char *d;
    u64 i;

    d = (char *)dst;
    for (i = 0; i < size; ++i)
        d[i] = ch;
}

void memmove(void *dst, const void *src, size_t size)
{
    char *d;
    char *s;
    s64 i;

    d = (char *)dst;
    s = (char *)src;
    for (i = size; i >= 0; --i)
        d[i] = s[i];
}

void bzero(void *p, size_t size)
{
    char *d;
    u64 i;

    d = (char *)p;
    for (i = 0; i < size; ++i)
        d[i] = 0;
}

void arch_flush_cache(u64 start, s64 len, int op_type)
{
    /* Not needed on x86. (at least so far) */
}

/*
 * memcpy with non-temporal hint, the processor does not write the data into
 * the cache hierarchy, nor does it fetch the corresponding cache line from
 * memory into the cache hierarchy. So clflush can be avoided.
 */
void memcpy_nt(void *dst, void *src, size_t len)
{
    int i;
    long long t1, t2, t3, t4;
    unsigned char *from, *to;
    size_t remain = len & 63;

    from = src;
    to = dst;
    i = len / 64;

    for (; i > 0; i--) {
        __asm__ __volatile__(
                "  mov (%4), %0\n"
                "  mov 8(%4), %1\n"
                "  mov 16(%4), %2\n"
                "  mov 24(%4), %3\n"
                "  movnti %0, (%5)\n"
                "  movnti %1, 8(%5)\n"
                "  movnti %2, 16(%5)\n"
                "  movnti %3, 24(%5)\n"
                "  mov 32(%4), %0\n"
                "  mov 40(%4), %1\n"
                "  mov 48(%4), %2\n"
                "  mov 56(%4), %3\n"
                "  movnti %0, 32(%5)\n"
                "  movnti %1, 40(%5)\n"
                "  movnti %2, 48(%5)\n"
                "  movnti %3, 56(%5)\n"
                : "=r"(t1), "=r"(t2), "=r"(t3), "=r"(t4), "+r"(from), "+r"(to)
                :
                : "memory");

        from += 64;
        to += 64;
    }

    /* Now do the tail of the block: */
    if (remain) {
        memcpy(to, from, remain);
        // arch_flush_cache(to, remain, 0);
    }

    // __asm__ __volatile__("mfence\n" : : );
}
