#include "testfs.h"
#include "block.h"
#ifndef DISABLE_RECON
#include "rv/rv.h"
#endif

static char zero[BLOCK_SIZE] = {0};

void
write_blocks(struct super_block *sb, char *blocks, int start, int nr)
{
        long pos;
#ifndef DISABLE_RECON        
        int i;
        for (i = 0; i < nr; i++) {
                rv_write(sb->rv, start + i, blocks + i * BLOCK_SIZE);
        }
#endif       
        if ((pos = ftell(sb->dev)) < 0) {
                EXIT("ftell");
        }
        if (fseek(sb->dev, start * BLOCK_SIZE, SEEK_SET) < 0) {
                EXIT("fseek");
        }
        if (fwrite(blocks, BLOCK_SIZE, nr, sb->dev) != nr) {
                EXIT("fwrite");
        }
        if (fseek(sb->dev, pos, SEEK_SET) < 0) {
                EXIT("fseek");
        }
}

void
zero_blocks(struct super_block *sb, int start, int nr)
{
        int i;

        for (i = 0; i < nr; i++) {
                write_blocks(sb, zero, start + i, 1);
        }
}

void
read_blocks(struct super_block *sb, char *blocks, int start, int nr)
{
        long pos;
        int i;

        if ((pos = ftell(sb->dev)) < 0) {
                EXIT("ftell");
        }
        if (fseek(sb->dev, start * BLOCK_SIZE, SEEK_SET) < 0) {
                EXIT("fseek");
        }
        if (fread(blocks, BLOCK_SIZE, nr, sb->dev) != nr) {
                EXIT("freed");
        }
        if (fseek(sb->dev, pos, SEEK_SET) < 0) {
                EXIT("fseek");
        }       
#ifndef DISABLE_RECON
        for (i = 0; i < nr; i++) {
                rv_read(sb->rv, start + i, blocks + i * BLOCK_SIZE);
        }
#else
        (void)i;
#endif
}

