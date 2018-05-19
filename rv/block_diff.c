#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <endian.h>
#include "rv.h"

/*
 * diff the old_block with new_block and store the difference in diff_block.
 * returns 0 if there is no difference, else 1.
 * returns negative value on error.
 */
int
block_diff(char *old_block, char *new_block, char *diff_block, int block_size)
{
        unsigned long *ob = (unsigned long *)old_block;
        unsigned long *nb = (unsigned long *)new_block;
        unsigned long *db = (unsigned long *)diff_block;
        int i, n = block_size/sizeof(unsigned long);
        int ret = 0;

        ASSERT((block_size % sizeof(unsigned long)) == 0);
        ASSERT(ob);
        ASSERT(nb);
        ASSERT(db);
        for (i = 0; i < n; i++) {
                db[i] = ob[i] ^ nb[i];
                if (db[i])
                        ret = 1;
        }
        return ret;
}

/*
 * returns the next bit that is 1 in diff_block.
 * returns -1 if there are no more 1 bits.
 * index should start with 0. 
 * for next call, index should be set to "prev return_value + 1".
 */
const int BITS_PER_INT = CHAR_BIT * sizeof(int);

int
block_next_diff(char *diff_block, int index, int block_size,
                int little_endian_bits)
{
        int i = index / BITS_PER_INT;
        int j = index % BITS_PER_INT;
        unsigned int mask;
        int orig_i = i;
        unsigned int orig_val = ((unsigned int *)diff_block)[orig_i];
        int ret = -1;
        int ints_per_block = block_size/sizeof(int);

        /* this function works at "int" (not long) granularity */
        /* we need to zero out some bits in the first int block */

        if (little_endian_bits) {
                mask = ((int)-1) << j;
                ((unsigned int *)diff_block)[orig_i] &= htole32(mask);
        } else {
                mask = (j == 0) ? -1 : ((int)1 << (BITS_PER_INT - j)) - 1;
                ((unsigned int *)diff_block)[orig_i] &= htobe32(mask);
        }

        for (; i < ints_per_block; i++) {
                unsigned int v = ((unsigned int *)diff_block)[i];
                if (!v) // fast path
                        continue;
                if (little_endian_bits) {
                        v = htole32(v);
                        ret = i * BITS_PER_INT + __builtin_ffs(v) - 1;
                } else {
                        v = htobe32(v);
                        ret = i * BITS_PER_INT + __builtin_clz(v);
                }
                goto out;
        }
out:
        /* undo the zeroing out of the first int block */
        ((int *)diff_block)[orig_i] = orig_val;
        return ret;
}

/* returns n'th bit in block.
   returns negative value on error. */
inline int
block_get_bit(char *block, unsigned int n, int block_size, 
              int little_endian_bits)
{
        unsigned int mask, v;
        ASSERT(n < block_size * CHAR_BIT);
        v = ((unsigned int *)block)[n/BITS_PER_INT];
        if (little_endian_bits) {
                mask = 1 << (n % BITS_PER_INT);
                v = htole32(v);
        } else {
                mask = 1 << (BITS_PER_INT - 1 - (n % BITS_PER_INT));
                v = htobe32(v);
        }
        return ((v & mask) != 0);

}

/* sets n'th bit in block to bit.
   returns negative value on error. */
int
block_set_bit(char *block, int n, unsigned int bit, int block_size,
              int little_endian_bits)
{
        unsigned int mask;
        ASSERT(n < block_size * CHAR_BIT);
        ASSERT(bit == 1 || bit == 0);
        if (little_endian_bits) {
                mask = 1 << (n % BITS_PER_INT);
                mask = htole32(mask);
        } else {
                mask = 1 << (BITS_PER_INT - 1 - (n % BITS_PER_INT));
                mask = htobe32(mask);
        }
        if (bit) {
                ((unsigned int *)block)[n/BITS_PER_INT] |= mask;
        } else {
                ((unsigned int *)block)[n/BITS_PER_INT] &= ~mask;
        }
        return 0;
}

/* Define the following and run "make block_diff". */
/* #define BLOCK_DIFF_TEST */
#ifdef BLOCK_DIFF_TEST
#include <stdlib.h>
#include <stdio.h>

#define BLOCK_SIZE 64

#define LEB 1
/* #define LEB 0 */

int
main()
{
        char block[BLOCK_SIZE] = {0};
        char nblock[BLOCK_SIZE] = {0};
        int n = -1;

        block[3] = 0x40;
        block[8] = 0x80;
        block[10] = 0x5c;
        block[20] = 0x38;
        block[23] = 0x38;

        while (1) {
                int ob;
                n = block_next_diff(block, n + 1, BLOCK_SIZE, LEB);
                if (n == -1)
                        break;
                ob = block_get_bit(block, n, BLOCK_SIZE, LEB);
                ASSERT(ob == 1);
                printf("n = %d\n", n);
                block_set_bit(nblock, n, ob, BLOCK_SIZE, LEB);
        }
        ASSERT(memcmp(block, nblock, BLOCK_SIZE) == 0);
        exit(0);
}
#endif /* BLOCK_DIFF_TEST */
