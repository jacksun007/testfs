#ifndef _BLOCK_DIFF_H
#define _BLOCK_DIFF_H

int block_diff(char *old_block, char *new_block, char *diff_block,
                  int block_size);
int block_next_diff(char *diff_block, int index, int block_size, 
                    int little_endian_bits);
int block_get_bit(char *block, unsigned int n, int block_size,
                  int little_endian_bits);
int block_set_bit(char *block, int n, unsigned int bit, int block_size,
                  int little_endian_bits);

#endif /* _BLOCK_DIFF_H */
