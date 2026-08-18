/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "plugin.h"

const uint8_t bs_log2_tab[256] = {
    0,0,1,1,2,2,2,2,3,3,3,3,3,3,3,3,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
    5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,5,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,6,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,
    7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7,7
};

/*
 * Rockbox's codec libraries normally call through the codec API (ci).  A
 * plugin has the equivalent services on rb instead, so provide the small C
 * runtime surface used by libfaad/libcodec without pulling in codeclib.o and
 * its codec-only ci dependency.
 */
void *memcpy(void *destination, const void *source, size_t bytes)
{
    return rb->memcpy(destination, source, bytes);
}

void *memmove(void *destination, const void *source, size_t bytes)
{
    return rb->memmove(destination, source, bytes);
}

void *memset(void *destination, int value, size_t bytes)
{
    return rb->memset(destination, value, bytes);
}

int memcmp(const void *left, const void *right, size_t bytes)
{
    return rb->memcmp(left, right, bytes);
}

void qsort(void *base, size_t count, size_t size,
           int (*compare)(const void *, const void *))
{
    rb->qsort(base, count, size, compare);
}
