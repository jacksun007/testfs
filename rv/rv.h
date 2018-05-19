#ifndef _RV_H
#define _RV_H

#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include "../list.h"
#include "../common.h"
#include "rbtree.h"

#ifndef DISABLE_LOGGING
#define RV_LOG(msg, n...) do { \
                char __str[1024];                        \
                snprintf(__str, 1024, msg, ##n);         \
                rv_log(__FUNCTION__, __str);             \
        } while (0)

#define RV_LOG_CHANGE(rv, msg, n...) do {                \
                char __str[1024];                        \
                snprintf(__str, 1024, msg, ##n);         \
                rv_log_change(rv, __FUNCTION__, __str);  \
        } while (0)
#else
#define RV_LOG(msg, n...)
#define RV_LOG_CHANGE(rv, msg, n...)
#endif
		
#define RET_ERROR(err)  do {                                            \
                RV_LOG("return: error=%d in %s at line %d", err,        \
                       __FILE__, __LINE__);                             \
                return err;                                             \
        } while (0)

#define ASSERT(expr) do {                                               \
                if (!(expr)) {                                          \
                        RV_LOG("%s: ASSERTION FAILED in %s at line %d", \
                               #expr, __FILE__, __LINE__);              \
                        return -EINVAL;                                 \
                }                                                       \
        } while (0)

#define RET_NOMEM(expr) do {                                            \
                if (!(expr)) {                                          \
                        RV_LOG("%s: ALLOCATION FAILED in %s at line %d", \
                               #expr, __FILE__, __LINE__);              \
                        return  -ENOMEM;                                \
                }                                                       \
        } while (0)

struct rv_block;
struct rv;

typedef int (*rv_tx_start_fn)(char *type);
typedef int (*rv_tx_end_fn)(void);
typedef int (*rv_block_fn)(struct rv_block *rvb);
typedef int (*rv_block_create_fn)(struct rv *rv, int nr, int write, 
                                  struct rv_block **rvbp);
typedef int (*rv_block_attach_fn)(struct rv_block *rvb, char *block);
typedef int (*rv_block_process_fn)(struct rv_block *nrvb, int *done);

struct rv_ops {
        rv_tx_start_fn tx_start;        /* invoked on tx start */
        rv_tx_end_fn tx_end;            /* invoked on tx end */

        rv_block_create_fn create;      /* create rv_block */
        rv_block_attach_fn attach;      /* attach block into rv_block */
        rv_block_fn destroy;            /* destroy rv_block */

        rv_block_fn invalidate;         /* invalidate block from rv_block */
        rv_block_fn read;               /* read block into rv_block */

        rv_block_fn references;         /* on read, get references in block */
        rv_block_fn preprocess;         /* on commit, pre-process block */
        rv_block_process_fn process;    /* on commit, process block */

        rv_block_fn corrupt;            /* corrupt block */
};

/* The XX is for private fields. This struct should only be accessed by rv.c.
 * However, we need to make the struct public because file systems inherit from
 * it. */
struct rv_block {
        struct hlist_node XXhnode;  /* linked list for hash table */
#ifdef CACHE_INVALIDATE
        struct list_head XXlnode;   /* linked list for lru cache */
        int XXrefcount;             /* for invalidation from lru cache */
#endif /* CACHE_INVALIDATE */
        int XXnr;                   /* block number */
        int XXflags;                /* various flags */
        struct rv_block *XXread;    /* version in read cache */
};

/* Used in get_flags below */
#define READ_CACHE      0x1
#define WRITE_CACHE     0x2
#define BOTH_CACHES     (READ_CACHE | WRITE_CACHE)

/* rv operations, called by higher-level file system or journaling layer */
int rv_init(FILE *dev, int corrupt, int block_threshold, struct rv **rvp);
void rv_read(struct rv *rv, int start, char *blocks);
void rv_write(struct rv *rv, int start, char *blocks);
void rv_tx_start(struct rv *rv, char *type);
void rv_tx_commit(struct rv *rv, char *type);
/* rv accessor functions */
struct rb_root *rv_get_rb_root(struct rv *rv);
FILE *rv_get_dev(struct rv *rv);

/* cache functions */
struct rv_block *rv_find_block(struct rv *rv, int nr, int flags);
int rv_add_block(struct rv *rv, int nr, int write, struct rv_block *rvb);
int rv_attach_block(struct rv *rv, struct rv_block *rvb);
int rv_remove_block(struct rv *rv, struct rv_block *rvb);

/* rv_block operations */
int rv_block_get(struct rv_block *rvb);
int rv_block_put(struct rv_block *rvb);
int rv_block_get_nr(struct rv_block *rvb);
int rv_block_get_processed(struct rv_block *rvb);
void rv_block_set_processed(struct rv_block *rvb);
struct rv_block *rv_block_get_prev_version(struct rv_block *nrvb);

/* logging functions */
int rv_log(const char *fname, char *msg);
int rv_log_change(struct rv *rv, const char *fname, char *msg);
#endif /* _RV_H */
