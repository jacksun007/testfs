#include <stdlib.h>
#include "../testfs.h"
#include "../super.h"
#include "../inode.h"
#include "../dir.h"
#include "rv_testfs.h"
#include "rv_interval.h"
#include "block_diff.h"
#ifndef DISABLE_PROLOG
#include "prolog.h"
#endif

static struct dsuper_block dsb;
static struct dsuper_block *sb = &dsb;
static struct rv *grv;
static char zero_block[BLOCK_SIZE];

typedef enum {
        SUPER_BLOCK = 0, /* first must be zero */
        INODE_FREEMAP,
        BLOCK_FREEMAP,
        INODE_BLOCK,
        INDIRECT_BLOCK,
        INDIRECT_DIR_BLOCK,
        DIR_BLOCK,
        UNKNOWN_BLOCK,
} testfs_block_type;

#ifndef DISABLE_LOGGING
static char *block_types_array[] = {
        "SUPER_BLOCK",
        "INODE_FREEMAP",
        "BLOCK_FREEMAP",
        "INODE_BLOCK",
        "INDIRECT_BLOCK",
        "INDIRECT_DIR_BLOCK",
        "DIR_BLOCK",
        "UNKNOWN_BLOCK"
};
#endif

struct dir_data {
        int inode_nr;
};

struct testfs_block_type_data {
        int ref_count;                          /* nr. of holders of struct */
        union {
                struct dir_data dir_block;      /* currently, only one type */
        } id;
};

struct testfs_block {
        struct rv_block base;                /* base type, MUST be first */
        char *blk;                           /* block data */
        /* fields below are copied in testfs_block_preprocess */
        testfs_block_type type;              /* type of testfs block */
        struct testfs_block_type_data *data; /* data for each block type */
};

struct processed_directory {
        struct list_head node;
        int dir_inode_nr;
};

struct deleted_block {
        struct list_head node;
        int bnr;
};

struct testfs_tx {
        struct list_head processed_directory_list;
        struct list_head deleted_block_list;
};

/* global variable accessed during a transaction */
static struct testfs_tx gtx;

/* add block bnr to the list of deleted blocks */
static int
delete_block(int bnr)
{
        struct deleted_block *db;
        RET_NOMEM(db = malloc(sizeof(struct deleted_block)));
        db->bnr = bnr;
        list_add(&db->node, &gtx.deleted_block_list);
        return 0;
}

static int testfs_block_create_typed(int nr, int write, testfs_block_type type, 
                                     struct testfs_block **tbp);
static int testfs_block_destroy(struct rv_block *rvb);

#ifndef DISABLE_LOGGING
static char *
block_type_string(testfs_block_type type)
{
        if (type < UNKNOWN_BLOCK)
                return block_types_array[type];
        else
                return block_types_array[UNKNOWN_BLOCK];
}
#endif

static void
to_int(char *str, int size, int *v)
{
        switch (size) {
        case 4:
                *v = *(int *)str;
                return;
        case 2:
                *v = *(short int *)str;
                return;
        case 1:
                *v = *(char *)str;
                return;
        }
}

static int
inode_to_nr(int nr, int i) {
        return ((nr - sb->inode_blocks_start) * INODES_PER_BLOCK + i);
}

/* returns negative value on error */
static int
inode_nr_to_block_nr(int inode_nr, int *bnrp) {
        int bnr = sb->inode_blocks_start + (inode_nr / INODES_PER_BLOCK);
        ASSERT(bnr < sb->data_blocks_start);
        *bnrp = bnr;
        return 0;
}

static struct testfs_block *
testfs_find(int nr, int get_flags)
{
        return (struct testfs_block *)rv_find_block(grv, nr, get_flags);
}

static int
testfs_put(struct testfs_block *tb)
{
        return rv_block_put((struct rv_block *)tb);
}

static int
testfs_block_init_data(struct testfs_block *tb, 
                       struct testfs_block_type_data *td)
{
        ASSERT(tb);
        ASSERT(!tb->data);
        ASSERT(td);
        td->ref_count = 1;
        tb->data = td;
        return 0;
}

static int
testfs_block_ref_data(struct testfs_block *tb, 
                      struct testfs_block_type_data *td)
{
        ASSERT(tb);
        ASSERT(!tb->data);
        if (td) {
                td->ref_count++;
        }
        tb->data = td;
        return 0;
}

static int
testfs_block_unref_data(struct testfs_block *tb)
{
        struct testfs_block_type_data *td;
        ASSERT(tb);
        td = tb->data;
        if (td) {
                if (--td->ref_count == 0) {
                        free(td);
                }
                tb->data = NULL;
        }
        return 0;
}

static int
dir_data_create(struct testfs_block *tb, int inode_nr)
{
        struct testfs_block_type_data *td;

        RET_NOMEM(td = malloc(sizeof(struct testfs_block_type_data)));
        td->id.dir_block.inode_nr = inode_nr;
        return testfs_block_init_data(tb, td);
}

/*
 * functions that find references in blocks
 */

/* finds references to inode_freemap, block_freemap and inode_blocks.
 * returns negative value on error. */
static int
super_block_refs(struct testfs_block *tb)
{
        struct rv_interval *rvi;
        struct rb_root *root = rv_get_rb_root(grv);
        int ret;

        *sb = *(struct dsuper_block *)tb->blk;
        ret = rv_interval_create(root, sb->inode_freemap_start, 
                                 sb->block_freemap_start, INODE_FREEMAP, &rvi);
        if (ret < 0)
                return ret;
        ret = rv_interval_create(root, sb->block_freemap_start, 
                                 sb->inode_blocks_start, BLOCK_FREEMAP, &rvi);
        if (ret < 0)
                return ret;
        ret = rv_interval_create(root, sb->inode_blocks_start,
                                 sb->data_blocks_start, INODE_BLOCK, &rvi);
        return ret;
}

/* finds references to DIR_BLOCK, INDIRECT_BLOCK and INDIRECT_DIR_BLOCK */
static int
inode_block_refs(struct testfs_block *tb)
{
        struct dinode *di;
        int i, j, ret;
        struct testfs_block *ntb;
        int nr = rv_block_get_nr((struct rv_block *)tb);

        ASSERT(tb->blk);
        for (i = 0; i < INODES_PER_BLOCK; i++) {
                unsigned int bnr;
                di = (struct dinode *)tb->blk + i;
                if (di->i_type == I_NONE)
                        continue;
                if (di->i_type == I_DIR) {
                        for (j = 0; j < NR_DIRECT_BLOCKS; j++) {
                                if ((bnr = di->i_block_nr[j]) == 0) {
                                        continue;
                                }
                                ret = testfs_block_create_typed(bnr, 0, 
                                                                DIR_BLOCK, 
                                                                &ntb);
                                if (ret < 0)
                                        return ret;
                                ret = dir_data_create(ntb, inode_to_nr(nr, i));
                                if (ret < 0)
                                        return ret;
                        }
                }
                ASSERT((di->i_type == I_FILE) || (di->i_type == I_DIR));
                if ((bnr = di->i_indirect) == 0) {
                        continue;
                }
                ret = testfs_block_create_typed(bnr, 0,
                                                (di->i_type == I_FILE) ?
                                                INDIRECT_BLOCK : 
                                                INDIRECT_DIR_BLOCK, &ntb);
                if (ret < 0)
                        return ret;
                ret = dir_data_create(ntb, inode_to_nr(nr, i));
                if (ret < 0)
                        return ret;
        }
        return 0;
}

/* finds references to DIR_BLOCK */
static int
indirect_dir_block_refs(struct testfs_block *tb)
{
        int i, ret;
        struct testfs_block *ntb;
        struct testfs_block_type_data *td = tb->data;

        ASSERT(tb->blk);
        ASSERT(td);
        for (i = 0; i < NR_INDIRECT_BLOCKS; i++) {
                unsigned int bnr = ((int *)tb->blk)[i];
                if (bnr > 0) {
                        ret = testfs_block_create_typed(bnr, 0, DIR_BLOCK, 
                                                        &ntb);
                        if (ret < 0)
                                return ret;
                        ret = dir_data_create(ntb, td->id.dir_block.inode_nr);
                        if (ret < 0)
                                return ret;
                }
        }
        return 0;
}

/*
 * functions that find differences in blocks
 */

typedef	int (*diff_fn)(struct rv_block *nrvb, int index, int struct_index, 
                       int old, int new);

struct struct_descriptor {
        int offset;
        char *name;
};

static int
diff_struct(diff_fn fn, struct rv_block *nrvb, int index, void *st, void *nst,
            struct struct_descriptor *sd)
{
        int i, ret;

        ASSERT(st);
        ASSERT(nst);
        ASSERT(sd);
        ASSERT(fn);
        for (i = 0; sd[i].name; i++) {
                int s = sd[i].offset;
                int e = sd[i+1].offset;
                if (memcmp((char *)st + s, (char *)nst + s, e - s)) {
                        int old;
                        int new;

                        to_int((char *)st + s, e - s, &old);
                        to_int((char *)nst + s, e - s, &new);
                        ret = fn(nrvb, index, i, old, new);
                        if (ret < 0)
                                return ret;
                }
        }
        return 0;
}

static struct struct_descriptor dsuper_descriptor[] = {
        {0, "inode_freemap_start"},
        {4, "block_freemap_start"},
        {8, "inode_blocks_start"}, 
        {12, "data_blocks_start"},
        {16, "modification_time"},
        {20, NULL}
};

/* returns 0 if diff_struct should continue */
static int
diff_super_fn(struct rv_block *nrvb, int index, int struct_index, int old, 
              int new) 
{
	RV_LOG_CHANGE(grv, "super_block, %s=%d, old=%d, new=%d", 
	  dsuper_descriptor[struct_index].name, struct_index, old,
	  new);
#ifndef DISABLE_PROLOG
	rv_prolog_assert("aiii", rv_super_block, struct_index, old,
	  new);
#endif
	return 0;
}

static int
diff_super_block(char *block, struct testfs_block *ntb)
{
        ASSERT(ntb->blk);
        *sb = *(struct dsuper_block *)ntb->blk;
        return diff_struct(diff_super_fn, (struct rv_block *)ntb, 0, block, 
                           ntb->blk, dsuper_descriptor);
}

static inline int
diff_freemap(diff_fn fn, struct rv_block *nrvb, char *block, char *nblock)
{
        int n = -1;
        char diff_block[BLOCK_SIZE];
        int ret;

        ASSERT(block);
        ASSERT(nblock);
        ret = block_diff(block, nblock, diff_block, BLOCK_SIZE);
        if (ret <= 0)
                return ret;
        while (1) {
                unsigned int ob, nb;
                n = block_next_diff(diff_block, n + 1, BLOCK_SIZE, 1);
                if (n == -1)
                        break;
                ob = block_get_bit(block, n, BLOCK_SIZE, 1);
                if (ob < 0)
                        return ob;
                nb = block_get_bit(nblock, n, BLOCK_SIZE, 1);
                if (nb < 0)
                        return nb;
                ASSERT(ob != nb);
                ret = fn(nrvb, 0, n, ob, nb);
                if (ret < 0)
                        return ret;
        }
        return 0;
}

static int
diff_inode_freemap_fn(struct rv_block *nrvb, int index, int struct_index,
                      int old, int new)
{
	RV_LOG_CHANGE(grv, "inode_freemap, inode_nr=%d, old=%d, new=%d",
                      struct_index, old, new);
#ifndef DISABLE_PROLOG
	rv_prolog_assert("aiii", rv_inode_freemap, struct_index, old, new);
#endif
        return 0;
}

static int
diff_inode_freemap(char *block, struct testfs_block *ntb)
{
        ASSERT(ntb->blk);
        return diff_freemap(diff_inode_freemap_fn, (struct rv_block *)ntb, 
                            block, ntb->blk);
}

static int
diff_block_freemap_fn(struct rv_block *nrvb, int index, int struct_index,
                      int old, int new)
{
        RV_LOG_CHANGE(grv, "block_freemap, block_nr=%d, old=%d, new=%d",
                      struct_index + sb->data_blocks_start, old, new);
#ifndef DISABLE_PROLOG
	rv_prolog_assert("aiii", rv_block_freemap, 
		struct_index + sb->data_blocks_start, old, new);
#endif
        return 0;
}

static int
diff_block_freemap(char *block, struct testfs_block *ntb)
{
        ASSERT(ntb->blk);
        return diff_freemap(diff_block_freemap_fn, (struct rv_block *)ntb, 
                            block, ntb->blk);
}

static struct struct_descriptor dinode_descriptor[] = {
        {0, "i_type"},
        {4, "i_mod_time"},
        {8, "i_size"}, 
        {12, "i_block_nr[0]"},
        {16, "i_block_nr[1]"},
        {20, "i_block_nr[2]"},
        {24, "i_block_nr[3]"},
        {28, "i_indirect"},
        {32, NULL}
};

/* returns 0 if diff_struct should continue */
static int
diff_inode_fn(struct rv_block *nrvb, int index, int struct_index, int old, 
              int new)
{
        int nr = rv_block_get_nr(nrvb);
        int inode_nr = inode_to_nr(nr, index);
        struct testfs_block *ntb = (struct testfs_block *)nrvb;
        struct dinode *di;
        struct testfs_block *dtb;
        int ret;

        ASSERT(ntb->blk);
        di = (struct dinode *)ntb->blk + index;
        ASSERT(old != new);
        if (struct_index >= 3 && struct_index <= 7) {
                ASSERT(old == 0 || new == 0);
                if (old) {
                        ret = delete_block(old);
                        if (ret < 0)
                                return ret;
                } else if ((struct_index >= 3 && struct_index <= 6) && 
                           di->i_type == I_DIR) {
                        dtb = testfs_find(new, WRITE_CACHE);
                        ASSERT(dtb);
                        dtb->type = DIR_BLOCK;
                        ret = dir_data_create(dtb, inode_nr);
                        testfs_put(dtb);
                        if (ret < 0)
                                return ret;
                } else if (struct_index == 7 && 
                           (di->i_type == I_DIR || di->i_type == I_FILE)) {
                        dtb = testfs_find(new, WRITE_CACHE);
                        ASSERT(dtb);
                        dtb->type = (di->i_type == I_FILE) ?
                                INDIRECT_BLOCK : INDIRECT_DIR_BLOCK;
                        ret = dir_data_create(dtb, inode_nr);
                        testfs_put(dtb);
                        if (ret < 0)
                                return ret;
                }
        }
        RV_LOG_CHANGE(grv, "inode, inode_nr=%d, %s=%d, old=%d, new=%d",
                      inode_nr, dinode_descriptor[struct_index].name, 
                      struct_index, old, new);
#ifndef DISABLE_PROLOG
	rv_prolog_assert("aiiii", rv_inode,
			inode_nr, struct_index, old, new);
#endif
        return 0;
}

static int
diff_inode_block(char *block, struct testfs_block *ntb)
{
        int i, ret;

        if (!block)
                block = zero_block;
        ASSERT(ntb);
        ASSERT(ntb->blk);
        for (i = 0; i < INODES_PER_BLOCK; i++) {
                ret = diff_struct(diff_inode_fn, (struct rv_block *)ntb, i,
                                  (struct dinode *)block + i, 
                                  (struct dinode *)ntb->blk + i, 
                                  dinode_descriptor);
                if (ret < 0)
                        return ret;
        }
        return 0;
}

static int
diff_indirect_block(char *block, struct testfs_block *ntb)
{
        int i;
        int nr = rv_block_get_nr((struct rv_block *)ntb);

        if (!block)
                block = zero_block;
        ASSERT(ntb);
        ASSERT(ntb->blk);
        (void)nr;

        for (i = 0; i < NR_INDIRECT_BLOCKS; i++) {
                unsigned int old = ((int *)block)[i];
                unsigned int new = ((int *)ntb->blk)[i];

                if (old == new)
                        continue;
                ASSERT(old == 0 || new == 0);
                RV_LOG_CHANGE(grv, "indirect_block, block_nr=%d, index=%d, "
                              "old=%d, new=%d", nr, i, old, new);
#ifndef DISABLE_PROLOG
                rv_prolog_assert("aiiii", rv_indirect_block,
                        nr, i, old, new);
#endif
        }
        return 0;
        
}

static int
diff_indirect_dir_block(char *block, struct testfs_block *ntb)
{
        int i;
        int nr;
        struct testfs_block_type_data *td = ntb->data;

        ASSERT(ntb);
        ASSERT(ntb->blk);
        ASSERT(td);
        (void)nr;       // remove warning if DISABLE_LOGGING
        
        if (!block)
                block = zero_block;
        nr = rv_block_get_nr((struct rv_block *)ntb);

        for (i = 0; i < NR_INDIRECT_BLOCKS; i++) {
                unsigned int old = ((int *)block)[i];
                unsigned int new = ((int *)ntb->blk)[i];
                struct testfs_block *dtb;
                int ret;

                if (old == new)
                        continue;
                ASSERT(old == 0 || new == 0);
                if (old) {
                        ret = delete_block(old);
                        if (ret < 0)
                                return ret;
                } else {
                        dtb = testfs_find(new, WRITE_CACHE);
                        ASSERT(dtb);
                        dtb->type = DIR_BLOCK;
                        ret = dir_data_create(dtb, td->id.dir_block.inode_nr);
                        testfs_put(dtb);
                        if (ret < 0)
                                return ret;
                }
                RV_LOG_CHANGE(grv, "indirect_dir_block, block_nr=%d, index=%d, "
                              "old=%d, new=%d", nr, i, old, new);
#ifndef DISABLE_PROLOG
		rv_prolog_assert("aiiii", rv_indirect_dir_block, 
			nr, i, old, new);
#endif
        }
        return 0;
        
}

static int
read_data(struct dinode *di, int flags, int offset, char *buf, int size)
{
        int block_offset = offset % BLOCK_SIZE;
        int copied = 0;

        while (copied < size) {
                int block_nr = (offset + copied) / BLOCK_SIZE;
                int sz;
                int bnr;
                struct testfs_block *tb;

                if (block_nr < NR_DIRECT_BLOCKS) {
                        bnr = di->i_block_nr[block_nr];
                } else {
                        block_nr -= NR_DIRECT_BLOCKS;
                        if (block_nr >= NR_INDIRECT_BLOCKS) {
                                RET_ERROR(-EFBIG);
                        }
                        if (di->i_indirect == 0)
                                RET_ERROR(-ENOENT);
                        tb = testfs_find(di->i_indirect, flags);
                        if (!tb || !tb->blk)
                                RET_ERROR(-ENOENT);
                        bnr = ((int *)(tb->blk))[block_nr];
                        testfs_put(tb);
                }
                if (bnr == 0)
                        RET_ERROR(-ENOENT);
                tb = testfs_find(bnr, flags);
                if (!tb || !tb->blk)
                        RET_ERROR(-ENOENT);
                if ((size - copied) <= (BLOCK_SIZE - block_offset)) {
                        sz = size - copied;
                } else {
                        sz = BLOCK_SIZE - block_offset;
                }
                memcpy(buf + copied, tb->blk + block_offset, sz);
                testfs_put(tb);
                copied += sz;
                block_offset = 0;
        }
        return 0;
}

/* returns negative value on error. */
static int
next_dirent(struct dinode *di, int flags, int *offset, struct dirent **dpp)
{
        struct dirent d;
        struct dirent *dp;
        int ret;

        ASSERT(*offset <= di->i_size);
        ret = read_data(di, flags, *offset, (char *)&d, sizeof(struct dirent));
        if (ret < 0)
                return ret;
        ASSERT(d.d_name_len > 0);
        ASSERT(d.d_name_len <= di->i_size);
        RET_NOMEM(dp = malloc(sizeof(struct dirent) + d.d_name_len));
        *dp = d;
        *offset += sizeof(struct dirent);
        ret = read_data(di, flags, *offset, D_NAME(dp), d.d_name_len);
        if (ret < 0) {
                free(dp);
                return ret;
        }
        *offset += d.d_name_len;
        *dpp = dp;
        return 0;
}

typedef	int (*dirent_fn)(int inode_nr, struct dirent *d, void *v);

/* returns negative value on error. */
static int
dir_iterate(int inode_nr, int flags, dirent_fn fn, void *v)
{
        int nr;
        struct testfs_block *tb;
        struct dinode *di;
        int offset;
        int ret;

        ret = inode_nr_to_block_nr(inode_nr, &nr);
        if (ret < 0)
                return ret;
        /* find inode block */
        ASSERT(fn);
        tb = testfs_find(nr, flags);
        if (!tb || !tb->blk)
                RET_ERROR(-ENOENT);
        di = (struct dinode *)tb->blk + (inode_nr % INODES_PER_BLOCK);
        if (di->i_type == I_NONE) {
                testfs_put(tb);
                RET_ERROR(-ENOENT);
        }
        if (di->i_type != I_DIR) {
                testfs_put(tb);
                RET_ERROR(-ENOTDIR);
        }
        offset = 0;
        while (offset < di->i_size) {
                struct dirent *d;

                ret = next_dirent(di, flags, &offset, &d);
                if (ret < 0)
                        goto out;
                if (d->d_inode_nr == -1) { /* deleted */
                        free(d);
                        continue;
                }
                ret = fn(inode_nr, d, v);
                if (ret < 0)
                        goto out;
        }
out:
        testfs_put(tb);
        return ret;
}

struct dir_hash {
        int flags;
        struct hlist_node dnode;
        struct dirent *dir;
};

#define DIR_HASH_SHIFT 8

#define dhashfn(nr)	\
	hash_int((unsigned int)nr, DIR_HASH_SHIFT)

static const int dir_hash_size = (1 << DIR_HASH_SHIFT);

/* returns negative value on error. */
static int
create_dir_cache(struct hlist_head **dcache)
{
        struct hlist_head *dir_cache;
        int i;

        RET_NOMEM(dir_cache = malloc(dir_hash_size * 
                                     sizeof(struct hlist_head)));
        for (i = 0; i < dir_hash_size; i++) {
                INIT_HLIST_HEAD(&dir_cache[i]);
        }
        *dcache = dir_cache;
        return 0;
}

/* returns negative value on error. */
static int
fill_dir_cache(int inode_nr, struct dirent *d, void *v)
{
        struct hlist_head *dcache = (struct hlist_head *)v;
        struct dir_hash *dh;

        RET_NOMEM(dh = malloc(sizeof(struct dir_hash)));
        INIT_HLIST_NODE(&dh->dnode);
        dh->flags = 0;
        hlist_add_head(&dh->dnode, &dcache[dhashfn(d->d_inode_nr)]);
        dh->dir = d;
        return 0;
}

static int
compare_directories(int inode_nr, struct dirent *d, void *v)
{
        struct hlist_node *elem;
        struct hlist_head *dcache = (struct hlist_head *)v;
        struct dir_hash *dh;

        hlist_for_each_entry(dh, elem, &dcache[dhashfn(d->d_inode_nr)], dnode) {
                if (strcmp(D_NAME(dh->dir), D_NAME(d)) == 0) {
                        dh->flags = 1; // found
                        return 0;
                }
        }
        RV_LOG_CHANGE(grv, "dir_block, add, dir_inode_nr=%d, name=%s, "
                      "inode_nr=%d, dirent_size=%lu", inode_nr, D_NAME(d), 
                      d->d_inode_nr, sizeof(struct dirent) + d->d_name_len);
#ifndef DISABLE_PROLOG
	rv_prolog_assert("aaisii", rv_dir_block, rv_action_add,
		inode_nr, D_NAME(d), d->d_inode_nr, 
		sizeof(struct dirent) + d->d_name_len);
#endif
        return 0;
}

/* read the entire directory to do the diff. */
static int
diff_dir_block(char *block, struct testfs_block *ntb)
{
        struct testfs_block_type_data *td;
        int inode_nr;
        int ret;
        int i;
        struct hlist_head *dcache;
        struct processed_directory *pd;

        ASSERT(ntb);
        td = ntb->data;
        ASSERT(td);
        inode_nr = td->id.dir_block.inode_nr;

        list_for_each_entry(pd, &gtx.processed_directory_list, node) {
                if (pd->dir_inode_nr == inode_nr) /* dir is processed */
                        return 0;
        }
        ret = create_dir_cache(&dcache);
        if (ret < 0)
                return ret;
        ret = dir_iterate(inode_nr, READ_CACHE, fill_dir_cache, dcache);
        if (ret < 0 && ret != -ENOENT) /* directory may not exist previously */
                return ret;
        ret = dir_iterate(inode_nr, BOTH_CACHES, compare_directories, dcache);
        if (ret < 0 && ret != -ENOENT) /* directory may not exist anymore */
                return ret;

        /* add this directory inode to the list of processed directories */
        RET_NOMEM(pd = malloc(sizeof(struct processed_directory)));
        pd->dir_inode_nr = inode_nr;
        list_add(&pd->node, &gtx.processed_directory_list);

        /* get rid of the dir cache */
        for (i = 0; i < dir_hash_size; i++) {
                struct dir_hash *dh;
                struct hlist_node *elem, *telem;

                hlist_for_each_entry_safe(dh, elem, telem, &dcache[i], dnode) {
                        if (dh->flags == 0) {
                                RV_LOG_CHANGE(grv, "dir_block, remove, "
                                              "dir_inode_nr=%d, name=%s, "
                                              "inode_nr=%d, dirent_size=%lu", 
                                              inode_nr, D_NAME(dh->dir), 
                                              dh->dir->d_inode_nr, 
                                              sizeof(struct dirent) + 
                                              dh->dir->d_name_len);
#ifndef DISABLE_PROLOG
				rv_prolog_assert("aaisii", rv_dir_block, 
				        rv_action_remove,
					inode_nr, D_NAME(dh->dir), 
					dh->dir->d_inode_nr, 
					sizeof(struct dirent) + 
					dh->dir->d_name_len);
#endif
                        }
                        free(dh->dir);
                        hlist_del(&dh->dnode);
                        free(dh);
                }
                ASSERT(hlist_empty(&dcache[i]));
        }
        free(dcache);
        return 0;
}

typedef	int (*testfs_block_references_fn)(struct testfs_block *tb);
typedef	int (*testfs_block_diff_fn)(char *block, struct testfs_block *tb);

struct testfs_block_type_functions {
        testfs_block_type type;
        testfs_block_references_fn references;
        testfs_block_diff_fn diff;
};

static struct testfs_block_type_functions testfs_block_table[] = {
        {SUPER_BLOCK,        super_block_refs,        diff_super_block},
        {INODE_FREEMAP,      NULL,                    diff_inode_freemap},
        {BLOCK_FREEMAP,      NULL,                    diff_block_freemap},
        {INODE_BLOCK,        inode_block_refs,        diff_inode_block},
        {INDIRECT_BLOCK,     NULL,                    diff_indirect_block},
        {INDIRECT_DIR_BLOCK, indirect_dir_block_refs, diff_indirect_dir_block},
        {DIR_BLOCK,          NULL,                    diff_dir_block},
};

static int
testfs_txn_start(char *type)
{
        INIT_LIST_HEAD(&gtx.processed_directory_list);
        INIT_LIST_HEAD(&gtx.deleted_block_list);
        return 0;
}

static int
testfs_txn_end(void)
{
	struct processed_directory *pd, *tpd;
	struct deleted_block *db, *tdb;

#ifndef DISABLE_PROLOG
	//rv_prolog_call("rule_A_fail", 3);
	rv_prolog_call("rule_B_fail", 3);
	rv_prolog_call("rule_C_fail", 2);
	rv_prolog_call("rule_6_fail", 1);
	rv_prolog_call("rule_7_fail", 2);
	rv_prolog_call("rule_8_fail", 2);
	rv_prolog_call("rule_9_fail", 1);
	rv_prolog_call("rule_10_fail", 1);
	rv_prolog_call("rule_20_or_21_fail", 4);
	rv_prolog_call("rule_20_or_21_fail", 3);
	rv_prolog_call("rule_23_fail", 2);
	rv_prolog_call("rule_32_fail", 3);
	rv_prolog_call("rule_36_fail", 0);
	rv_prolog_retractall();
#endif

	list_for_each_entry_safe(pd, tpd, &gtx.processed_directory_list, 
							 node) {
			list_del(&pd->node);
			free(pd);
	}
	ASSERT(list_empty(&gtx.processed_directory_list));

	list_for_each_entry_safe(db, tdb, &gtx.deleted_block_list, node) {
			struct testfs_block *tb = testfs_find(db->bnr, READ_CACHE);
			if (tb) {
					testfs_put(tb);
					testfs_block_destroy((struct rv_block *)tb);
			}
			list_del(&db->node);
			free(db);
	}
	ASSERT(list_empty(&gtx.deleted_block_list));
						
	return 0;
}

static int
testfs_block_create_typed(int nr, int write, testfs_block_type type, 
                          struct testfs_block **tbp)
{
        struct testfs_block *tb;

        tb = testfs_find(nr, write ? WRITE_CACHE : READ_CACHE);
        if (tb) {
                testfs_put(tb);
                RET_ERROR(-EEXIST);
        }
        RET_NOMEM(tb = malloc(sizeof(struct testfs_block)));
        /* sets up base field in struct testfs_block */
        rv_add_block(grv, nr, write, (struct rv_block *)tb);
        tb->type = type;
        tb->blk = NULL;
        tb->data = NULL;
        *tbp = tb;
        return 0;
}

/* on a read, create a block only if its type is known.
 * on a write, create a block of unknown type. */
static int
testfs_block_create(struct rv *rv, int nr, int write, struct rv_block **rvbp)
{
        testfs_block_type type = UNKNOWN_BLOCK;
        struct rb_root *root;

        ASSERT(rvbp);
        ASSERT(write == 0 || write == 1);
        ASSERT(rv == grv);
        root = rv_get_rb_root(grv);
        if (!write) {
                struct rv_interval *rvi = rv_interval_find(root, nr);
                if (!rvi) { /* nothing known about block */
                        *rvbp = NULL; /* caller should check this value */
                        return 0;
                }
                type = rv_interval_type(rvi);
        }
        return testfs_block_create_typed(nr, write, type, 
                                         (struct testfs_block **)rvbp);
}

/* make a copy of the block */
static int
testfs_block_attach(struct rv_block *rvb, char *block)
{
        struct testfs_block *tb = (struct testfs_block *)rvb;

        ASSERT(!tb->blk);
        RET_NOMEM(tb->blk = malloc(BLOCK_SIZE));
        RV_LOG("block_nr = %d, type = %s", rv_block_get_nr(rvb),
               block_type_string(tb->type));
        memcpy(tb->blk, block, BLOCK_SIZE);
        return rv_attach_block(grv, rvb);
}

static int
testfs_block_destroy(struct rv_block *rvb)
{
        struct testfs_block *tb = (struct testfs_block *)rvb;
        int ret;

        ASSERT(rvb);
        RV_LOG("block_nr = %d, type = %s", rv_block_get_nr(rvb),
               block_type_string(tb->type));
        /* TODO: return value ignored */
        ret = rv_remove_block(grv, rvb);
        ret = testfs_block_unref_data(tb);
        free(tb->blk);
        tb->blk = NULL;
        return ret;
}

/* this function must only be called by the rv code and not from this file. */
static int
testfs_block_read(struct rv_block *rvb)
{
        struct testfs_block *tb = (struct testfs_block *)rvb;
        int nr;
        long pos;
        int ret;
        FILE *dev;

        ASSERT(!tb->blk);
        RET_NOMEM(tb->blk = malloc(BLOCK_SIZE));
        RV_LOG("block_nr = %d, type = %s", rv_block_get_nr(rvb),
               block_type_string(tb->type));
        nr = rv_block_get_nr(rvb);
        dev = rv_get_dev(grv);
        if ((pos = ftell(dev)) < 0) {
                RET_ERROR((int)pos);
        }
        if ((ret = fseek(dev, nr * BLOCK_SIZE, SEEK_SET)) < 0) {
                RET_ERROR(ret);
        }
        if ((ret = fread(tb->blk, BLOCK_SIZE, 1, dev)) != 1) {
                RET_ERROR(-errno);
        }
        if ((ret = fseek(dev, pos, SEEK_SET)) < 0) {
                RET_ERROR(ret);
        }
        return 0;
}

/* this function must only be called by the rv code and not from this file. */
static int
testfs_block_invalidate(struct rv_block *rvb)
{
        struct testfs_block *tb = (struct testfs_block *)rvb;

        ASSERT(rvb);
        RV_LOG("block_nr = %d, type = %s", rv_block_get_nr(rvb),
               block_type_string(tb->type));
        free(tb->blk);
        tb->blk = NULL;
        return 0;
}

static int
testfs_block_references(struct rv_block *rvb)
{
        int ret;
        struct testfs_block *tb = (struct testfs_block *)rvb;
        ASSERT(tb->type >= 0);
        ASSERT(tb->type < UNKNOWN_BLOCK);
        RV_LOG("block_nr = %d, type = %s", rv_block_get_nr(rvb), 
               block_type_string(tb->type));
        rv_block_set_processed(rvb);
        if (!testfs_block_table[tb->type].references)
                return 0;
        ret = testfs_block_table[tb->type].references(tb);
        return ret;
}

static int
testfs_block_preprocess(struct rv_block *nrvb)
{
        struct testfs_block *ntb = (struct testfs_block *)nrvb;
        struct testfs_block *tb;
        int nr = rv_block_get_nr(nrvb);
        int ret;

        tb = (struct testfs_block *)rv_block_get_prev_version(nrvb);
        
        if (tb) { /* copy certain fields into ntb */
                ntb->type = tb->type;
                ret = testfs_block_ref_data(ntb, tb->data);
                if (ret < 0)
                        return ret;
        } else {
                struct rv_interval *rvi;
                struct rb_root *root = rv_get_rb_root(grv);

                rvi = rv_interval_find(root, nr);
                if (rvi) {
                        ntb->type = rv_interval_type(rvi);
                }
        }
        return 0;
}

static int
testfs_block_process(struct rv_block *nrvb, int *done)
{
        struct testfs_block *ntb = (struct testfs_block *)nrvb;
        int ret;
        int __attribute__((unused)) nr = rv_block_get_nr(nrvb);
        struct testfs_block *tb;

        ASSERT(ntb->type >= 0);
        ASSERT(ntb->type <= UNKNOWN_BLOCK);

        if (ntb->type == UNKNOWN_BLOCK)
                return 0;
        *done = 1; /* type is known, we will process this block */
        rv_block_set_processed(nrvb);
        RV_LOG("block_nr = %d, type = %s", nr, block_type_string(ntb->type));
        if (!testfs_block_table[ntb->type].diff)
                return 0;
        tb = (struct testfs_block *)rv_block_get_prev_version(nrvb);
        ret = testfs_block_table[ntb->type].diff(tb ? tb->blk : NULL, ntb);
        return ret;
}

/* do random corruption of block */
static int
testfs_block_corrupt(struct rv_block *rvb)
{
        struct testfs_block *tb = (struct testfs_block *)rvb;
        int i, j, n;
        unsigned int start, end;

#define RAND(max) (long)(((float)rand() / RAND_MAX) * max)

        if (!tb->blk)
                return 0;
        n = RAND(10);
        n -= 7;
        for (i = 0; i < n; i++) {
                start =  RAND(BLOCK_SIZE);
                end = start + RAND((BLOCK_SIZE - start));
                ASSERT(start < BLOCK_SIZE);
                ASSERT(end < BLOCK_SIZE);
                for (j = start; j < end; j++) {
                        tb->blk[j] = RAND(256);
                }
        }
        return 0;
}

struct rv_ops testfs_block_ops = {
        testfs_txn_start,
        testfs_txn_end,

        testfs_block_create,
        testfs_block_attach,
        testfs_block_destroy,

        testfs_block_invalidate,
        testfs_block_read,

        testfs_block_references,
        testfs_block_preprocess,
        testfs_block_process,

        testfs_block_corrupt,
};

/* returns negative value on error */
int
rv_testfs_init(struct rv *rv, struct rv_ops **opsp, int *consistent,
               int *multiple_updates)
{
        int ret;
        struct rv_interval *rvi;
        struct rb_root *root = rv_get_rb_root(rv);

        grv = rv;
        ret = rv_interval_create(root, 0, 1, SUPER_BLOCK, &rvi);
        if (ret < 0)
                return ret;
        *opsp = &testfs_block_ops;
        /* this file system is not crash consistent */
        *consistent = 0;
        /* a block can be updated multiple times in a transaction */
        *multiple_updates = 1;
        return 0;
}

struct dsearch_t
{
	char * name;
	int inode_nr;
};
		
#ifndef DISABLE_PROLOG
static int find_child_inode_by_name(int inode_nr, struct dirent *d, void *v)
{
	struct dsearch_t * info = (struct dsearch_t *)v;
	//printf("inode_nr: %d, name: %s\n", inode_nr, D_NAME(d));
	if ( strcmp( D_NAME(d), info->name ) == 0 )
	{
		info->inode_nr = d->d_inode_nr;
	}
	
	return 0;
}
	
static foreign_t rv_testfs_dir_get_child(term_t din, term_t name, term_t in)
{
	int inode_nr;
	struct dsearch_t info = { NULL, -1 };
	PL_ASSERT(PL_get_integer(din, &inode_nr));
	PL_ASSERT(PL_get_atom_chars(name, &info.name));
	//printf("dir_get_child(%d, '%s', _)\n", inode_nr, info.name); 
	if ( dir_iterate(inode_nr, READ_CACHE, 
		find_child_inode_by_name, &info) >= 0 )
	{
		if ( info.inode_nr >= 0 )
		{
			//printf("dir found: inode_nr=%d\n", info.inode_nr);
			return PL_unify_integer(in, (long)info.inode_nr);
		}
	}
	//printf("dir not found.\n");
	PL_fail;
}

	
static foreign_t rv_testfs_inode_get(term_t in, term_t field, term_t out)
{
	struct testfs_block *dtb;
	int inode_nr, block_nr;
	int offset;
	PL_ASSERT(PL_get_integer(field, &offset));
	if ( offset >= 0 && offset <= 7 )
	{
		PL_ASSERT(PL_get_integer(in, &inode_nr));
		inode_nr_to_block_nr(inode_nr, &block_nr);
		dtb = testfs_find(block_nr, READ_CACHE);
		if ( dtb->type == INODE_BLOCK )
		{
			int * array = (int *)dtb->blk;
			return PL_unify_integer(out, (long)array[offset]);
		}
	}
	PL_fail;
}
	
PL_extension rv_testfs_predicates[] =
{
/*{ "name",      	arity,  function,      			PL_FA_<flags> },*/
  { "dir_get_child", 	3,      rv_testfs_dir_get_child,  	0 },
  { "inode_get",        3,      rv_testfs_inode_get,       	0 },
  { NULL,        		0,      NULL,          				0 }
};

#endif

