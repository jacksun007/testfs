#include <time.h>
#include <ctype.h>
#include "rv.h"
#include "rv_interval.h"
#include "rv_testfs.h"
#ifndef DISABLE_PROLOG
#include "prolog.h"
#endif

/* flags for rv_block */
#define RV_BLOCK_PROCESSED      0x1
#define RV_BLOCK_ATTACHED       0x2

#ifdef CACHE_INVALIDATE
/* block was previously read into cache, but is currently on disk */
#define RV_BLOCK_ON_DISK        0x4
/* block is being read from disk */
#define RV_BLOCK_BEING_READ     0x8
#endif /* CACHE_INVALIDATE */


#ifdef CACHE_INVALIDATE
/* currently, all dummy */
#define LOCK_TYPE int
#define CONDITION_TYPE int
#define LOCK(lock)
#define UNLOCK(lock)
#define WAIT(cond, lock)
#define SIGNAL(cond)
#endif /* CACHE_INVALIDATE */

struct rv {
        int rv_enabled;
        int rv_corrupt;
        struct hlist_head *read_cache;
        struct hlist_head *write_cache;
#ifdef CACHE_INVALIDATE
        FILE *dev;
        /* A block (rvb) is in the LRU list (lru_head) under the following
         * constraint:
         * IN_LRU <=> (RV_BLOCK_ATTACHED && !RV_BLOCK_ON_DISK)
         * Also,
         * (XXrefcount > 0) => (RV_BLOCK_ATTACHED && !RV_BLOCK_ON_DISK)
         */
        struct list_head lru_head;
        int rv_block_threshold; /* target nr. of blocks in memory */
        int rv_blocks_in_memory; /* nr. of blocks currently in memory */
        LOCK_TYPE read_cache_lock;
        CONDITION_TYPE read_cache_cond;
#endif /* CACHE_INVALIDATE */
        struct rb_root rb_root;
        int rv_in_tx;
        int tx_id;               /* id of transaction */
        int fs_is_crash_consistent;
        int fs_multiple_block_updates;
        struct rv_ops ops;
};

static void rv_disable(struct rv *rv, int ret);
static int rv_init_cache(struct rv *rv);
static int rv_destroy_cache(struct rv *rv);

static int rv_process_transaction(struct rv *rv, char *type);
static int rv_block_get_attached(struct rv_block *rvb);
static void rv_block_set_attached(struct rv_block *rvb);

#ifdef CACHE_INVALIDATE
static int rv_block_get_on_disk(struct rv_block *rvb);
static void rv_block_set_on_disk(struct rv_block *rvb, int on_disk);
static int rv_block_get_being_read(struct rv_block *rvb);
static void rv_block_set_being_read(struct rv_block *rvb, int being_read);
#endif /* CACHE_INVALIDATE */

/* rv operations */
int
rv_init(FILE *dev, int corrupt, int block_threshold, struct rv **rvp)
{
        struct rv *rv;
        struct rv_ops *ops;
        int ret;
        int consistent;
        int multiple_updates;
        unsigned long i;

        srand(time(NULL));
        RET_NOMEM(rv = malloc(sizeof(struct rv)));
        bzero(rv, sizeof(struct rv));
#ifdef CACHE_INVALIDATE
        if (!dev)
                return -EINVAL;
        rv->dev = dev;
        INIT_LIST_HEAD(&rv->lru_head);
#endif /* CACHE_INVALIDATE */
        ret = rv_init_cache(rv);
        if (ret < 0)
                return ret;
        rv->rb_root = RB_ROOT;
#ifndef DISABLE_PROLOG
	rv_prolog_init();
#endif
        ret = rv_testfs_init(rv, &ops, &consistent, &multiple_updates);
        if (ret < 0) {
                goto destroy;
        }
        rv->fs_is_crash_consistent = consistent;
        rv->fs_multiple_block_updates = multiple_updates;

        if (!ops) {
                ret = -EINVAL;
                goto destroy;
        }                
        for (i = 0; i < sizeof(struct rv_ops)/sizeof(long); i++) {
                long *ip = ((long *)ops) + i;
                if (*ip == 0) {
                        ret = -EINVAL;
                        goto destroy;
                }
        }
        rv->ops = *ops;
        rv->rv_enabled = 1;
        rv->rv_corrupt = corrupt;
#ifdef CACHE_INVALIDATE
        ASSERT(block_threshold >= 0);
        rv->rv_block_threshold = block_threshold;
        rv->rv_blocks_in_memory = 0;
#endif /* CACHE_INVALIDATE */
        *rvp = rv;
        return 0;
destroy:
        rv_disable(rv, ret);
        return ret;
}

static void
rv_disable(struct rv *rv, int ret)
{
        rv_destroy_cache(rv);
        rv_interval_destroy(&rv->rb_root);
        errno = ret;
        rv->rv_enabled = 0;
        RV_LOG("WARN: rv_enabled is set to 0");
}

void
rv_read(struct rv *rv, int nr, char *block)
{
        struct rv_block *rvb;
        int ret;

        if (!rv->rv_enabled)
                return;

        rvb = rv_find_block(rv, nr, WRITE_CACHE);
        if (rvb) {
                rv_block_put(rvb);
                if (rv->fs_is_crash_consistent) {
                        /* a consistent fs should not read modified data */
                        ret = -EIO;
                        goto disable;
                } else {
                        /* the modified data will be available at the end of 
                         * the transaction. */
                        return;
                }
        }
        rvb = rv_find_block(rv, nr, READ_CACHE);
        if (!rvb) {
                /* add block to read cache */
                ret = rv->ops.create(rv, nr, 0, &rvb);
                if (ret < 0) {
                        goto disable;
                }
                if (!rvb)  /* likely data block */
                        return;
        }
        if (rv_block_get_attached(rvb)) {
                rv_block_put(rvb);
                return; /* block is already attached */
        }
        ret = rv->ops.attach(rvb, block);
        if (ret < 0)
                goto disable;
        ret = rv->ops.references(rvb);
        rv_block_put(rvb);
        if (ret < 0)
                goto disable;
        return;
disable:
        rv_disable(rv, ret);
} 

void
rv_write(struct rv *rv, int nr, char *block)
{
        int ret;
        struct rv_block *rvb;

        if (!rv)
                return;
        if (!rv->rv_enabled)
                return;
        if (!rv->rv_in_tx) {
                ret = -EINVAL;
                goto disable;
        }
        rvb = rv_find_block(rv, nr, WRITE_CACHE);
        if (rvb) {
                rv_block_put(rvb);
                if (!rv->fs_multiple_block_updates) { /* this is disallowed */
                        ret = -EIO;
                        goto disable;
                } else { /* remove current block */
                        rv->ops.destroy(rvb);
                }
                rvb = NULL;
        }
#ifdef CACHE_INVALIDATE
        if (!rv->fs_is_crash_consistent) {
                /* make sure that if the block exists, then it is valid in the
                 * read cache. otherwise, we may read an updated block into the
                 * read cache. see rv_invalidate_blocks(). */
                rvb = rv_find_block(rv, nr, READ_CACHE);
                if (rvb)
                        rv_block_put(rvb);
        }
#endif /* CACHE_INVALIDATE */
        /* add block to write cache */
        ret = rv->ops.create(rv, nr, 1, &rvb);
        if (ret < 0) {
                goto disable;
        }
        if (!rvb) {
                ret = -EINVAL;
                goto disable;
        }
        ret = rv->ops.attach(rvb, block);
        if (ret < 0) {
                goto disable;
        }
        if (rv->rv_corrupt) {
                ret = rv->ops.corrupt(rvb);
                if (ret < 0) {
                        rv_block_put(rvb);
                        goto disable;
                }
        }
        rv_block_put(rvb);
        return;
disable:
        rv_disable(rv, ret);
}

void
rv_tx_start(struct rv *rv, char *type)
{
        if (!rv)
                return;
        if (!rv->rv_enabled)
                return;
        RV_LOG("type = %s", type);
        rv->rv_in_tx = 1;
}

/* handle blocks enqueued within transaction */
void
rv_tx_commit(struct rv *rv, char *type)
{
        int ret;

        if (!rv)
                return;
        if (!rv->rv_enabled)
                return;
        rv->rv_in_tx = 0;
        ret = rv_process_transaction(rv, type);
        if (ret < 0)
                goto disable;
        return;
disable:
        rv_disable(rv, ret);
}

struct rb_root *
rv_get_rb_root(struct rv *rv)
{
        return &rv->rb_root;
}

FILE *
rv_get_dev(struct rv *rv)
{
        return rv->dev;
}

#ifdef CACHE_INVALIDATE
static int
rv_blocks_above_threshold(struct rv *rv)
{
        if (rv->rv_block_threshold == 0) {
                return 0;
        } 
        return rv->rv_blocks_in_memory >= rv->rv_block_threshold;
}
#endif /* CACHE_INVALIDATE */

/* cache functions */
#define BLOCK_HASH_SHIFT 8

#define bhashfn(nr)                                     \
	hash_int((unsigned int)nr, BLOCK_HASH_SHIFT)

static const int block_hash_size = (1 << BLOCK_HASH_SHIFT);

/* returns negative value on error */
static int
rv_init_cache(struct rv *rv)
{
        int i;

        RET_NOMEM(rv->read_cache = malloc(block_hash_size * 
                                          sizeof(struct hlist_head)));
        for (i = 0; i < block_hash_size; i++) {
                INIT_HLIST_HEAD(&rv->read_cache[i]);
        }
        RET_NOMEM(rv->write_cache = malloc(block_hash_size * 
                                           sizeof(struct hlist_head)));
        for (i = 0; i < block_hash_size; i++) {
                INIT_HLIST_HEAD(&rv->write_cache[i]);
        }
        return 0;
}

static int
rv_destroy_cache(struct rv *rv)
{
        int i;
        struct rv_block *rvb;
        struct hlist_node *elem, *telem;

        if (!rv->read_cache) {
                goto write_cache;
        }
        for (i = 0; i < block_hash_size; i++) {
                hlist_for_each_entry_safe(rvb, elem, telem,
                                          &rv->read_cache[i], XXhnode) {
                        /* this will remove rvb from read cache */
                        rv->ops.destroy(rvb);
                }
                ASSERT(hlist_empty(&rv->read_cache[i]));
        }
        free(rv->read_cache);

write_cache:
        if (!rv->write_cache) {
                goto end;
        }
        for (i = 0; i < block_hash_size; i++) {
                hlist_for_each_entry_safe(rvb, elem, telem,
                                          &rv->write_cache[i], XXhnode) {
                        rvb->XXread = NULL;
                        /* this will remove rvb from write cache */
                        rv->ops.destroy(rvb);
                }
                ASSERT(hlist_empty(&rv->write_cache[i]));
        }
        free(rv->write_cache);

end:
        return 0;
}

/* for debugging */
void
rv_print_cache(struct rv *rv)
{
        int i;
        struct hlist_node *elem;
        struct rv_block *rvb;

        if (!rv->read_cache) {
                RV_LOG("WARN: read_cache is NULL");
        } else {
                for (i = 0; i < block_hash_size; i++) {
                        hlist_for_each_entry(rvb, elem, &rv->read_cache[i],
                                             XXhnode) {
#ifdef CACHE_INVALIDATE
                                RV_LOG("read_cache: line = %d: nr = %d, "
                                       "ref = %d", i, rv_block_get_nr(rvb),
                                       rvb->XXrefcount);
#else /* CACHE_INVALIDATE */
                                RV_LOG("read_cache: line = %d: nr = %d", i, 
                                       rv_block_get_nr(rvb));
#endif /* CACHE_INVALIDATE */
                        }
                }
        }

        if (!rv->write_cache) {
                RV_LOG("WARN: write_cache is NULL");
        } else {
                for (i = 0; i < block_hash_size; i++) {
                        hlist_for_each_entry(rvb, elem, &rv->write_cache[i],
                                             XXhnode) {
#ifdef CACHE_INVALIDATE
                                RV_LOG("write_cache: line = %d: nr = %d, "
                                       "ref = %d", i, rv_block_get_nr(rvb),
                                       rvb->XXrefcount);
#else /* CACHE_INVALIDATE */
                                RV_LOG("write_cache: line = %d: nr = %d", i,
                                       rv_block_get_nr(rvb));
#endif /* CACHE_INVALIDATE */
                        }
                }
        }
}

#ifdef CACHE_INVALIDATE
static int
rv_read_block_from_disk(struct rv *rv, struct rv_block *rvb)
{
        int err;

        LOCK(rv->read_cache_lock);
restart:
        if (rv_block_get_on_disk(rvb)) {
                if (rv_block_get_being_read(rvb)) {
                        WAIT(rv->read_cache_cond, rv->read_cache_lock);
                        goto restart;
                }
                rv_block_set_being_read(rvb, 1);
                UNLOCK(rv->read_cache_lock);
                err = rv->ops.read(rvb);
                LOCK(rv->read_cache_lock);
                rv->rv_blocks_in_memory++;
                rv_block_set_on_disk(rvb, 0);
                rv_block_set_being_read(rvb, 0);
                /* add to front of lru */
                list_add(&rvb->XXlnode, &rv->lru_head);
                SIGNAL(rv->read_cache_cond);
        } else {
                /* move to front of lru */
                list_del(&rvb->XXlnode);
                list_add(&rvb->XXlnode, &rv->lru_head);
        }
        /* TODO: error is ignored */
        rv_block_get(rvb);
        UNLOCK(rv->read_cache_lock);
        return 0;
        
        (void)err;
}
#endif /* CACHE_INVALIDATE */

struct rv_block *
rv_find_block(struct rv *rv, int nr, int get_flags)
{
        struct hlist_node *elem;
        struct rv_block *rvb;

        if (get_flags & WRITE_CACHE) {
                hlist_for_each_entry(rvb, elem, &rv->write_cache[bhashfn(nr)],
                                     XXhnode) {
                        if (rv_block_get_nr(rvb) == nr) {
#ifdef CACHE_INVALIDATE
                                /* write_cache blocks are not invalidated */
                                if (!rv_block_get_attached(rvb))
                                        return rvb;
                                /* TODO: error is ignored */
                                rv_block_get(rvb);
#endif /* CACHE_INVALIDATE */
                                return rvb;
                        }
                }
        }
        if (get_flags & READ_CACHE) {
                hlist_for_each_entry(rvb, elem, &rv->read_cache[bhashfn(nr)], 
                                     XXhnode) {
                        if (rv_block_get_nr(rvb) == nr) {
#ifdef CACHE_INVALIDATE
                                if (!rv_block_get_attached(rvb))
                                        return rvb;
                                /* TODO: error is ignored */
                                rv_read_block_from_disk(rv, rvb);
#endif /* CACHE_INVALIDATE */
                                return rvb;
                        }
                }
        }
	return NULL;
}

/* this function must be called by rv->ops.create() to add the newly created
 * block to the cache. code in this file should not call this function. Instead,
 * it should call rv->ops.create().
 *
 * write should be 1 if the create occurs during write, 0 if the create occurs
 * during read.  returns negative value on error */
int
rv_add_block(struct rv *rv, int nr, int write, struct rv_block *rvb)
{
        struct hlist_head *head;

        INIT_HLIST_NODE(&rvb->XXhnode);
        head = write ? &rv->write_cache[bhashfn(nr)] : 
                &rv->read_cache[bhashfn(nr)];
        hlist_add_head(&rvb->XXhnode, head);
#ifdef CACHE_INVALIDATE
        INIT_LIST_HEAD(&rvb->XXlnode);
        rvb->XXrefcount = 0;
#endif /* CACHE_INVALIDATE */
        rvb->XXnr = nr;
        rvb->XXflags = 0;
        rvb->XXread = 0;
        return 0;
}

/* this function must be called by rv->ops.attach() to indicate that the block
 * data has been attached to the rv_block. Code in this file should not call
 * this function. Instead, it should call rv->ops.attach(). */
int
rv_attach_block(struct rv *rv, struct rv_block *rvb)
{
        ASSERT(rv);
        ASSERT(rvb);
#ifdef CACHE_INVALIDATE
        ASSERT(!rvb->XXrefcount);
        rvb->XXrefcount = 1;
        list_add(&rvb->XXlnode, &rv->lru_head);
        rv->rv_blocks_in_memory++;
#endif /* CACHE_INVALIDATE */
        rv_block_set_attached(rvb);
        return 0;
}

/* this function must be called by rv->ops.destroy() to remove the destroyed
 * block from the cache. Code in this file should not call this
 * function. Instead, it should call rv->ops.destroy(). */
int
rv_remove_block(struct rv *rv, struct rv_block *rvb)
{
        ASSERT(rv);
        ASSERT(rvb);
        ASSERT(!rvb->XXread);
#ifdef CACHE_INVALIDATE
        ASSERT(!rvb->XXrefcount);
        if (rv_block_get_attached(rvb) && !rv_block_get_on_disk(rvb)) {
                list_del(&rvb->XXlnode);
        }
#endif /* CACHE_INVALIDATE */
        hlist_del(&rvb->XXhnode);
        return 0;
}

#ifdef CACHE_INVALIDATE
/* must only be called at the end of transaction. otherwise, if an fs is not
 * crash consistent, then when a block is updated, we would read the updated
 * block in the read cache.  */
static int
rv_invalidate_blocks(struct rv *rv)
{
        int ret = 0;
        struct rv_block *rvb, *trvb;

        if (!rv_blocks_above_threshold(rv)) {
                return 0;
        }
        LOCK(rv->read_cache_lock);
        list_for_each_entry_safe(rvb, trvb, &rv->lru_head, XXlnode) {
                if (rvb->XXrefcount > 0)
                        continue;
                ASSERT(rv_block_get_attached(rvb));
                ASSERT(!rv_block_get_on_disk(rvb));
                ret = rv->ops.invalidate(rvb);
                if (ret < 0)
                        goto out;
                rv_block_set_on_disk(rvb, 1);
                /* remove from lru list */
                list_del(&rvb->XXlnode);
                rv->rv_blocks_in_memory--;
                if (!rv_blocks_above_threshold(rv))
                        break;
        }
out:
        UNLOCK(rv->read_cache_lock);
        return ret;
}
#endif /* CACHE_INVALIDATE */

/* work horse */
static int
rv_process_transaction(struct rv *rv, char *type)
{
        int i;
        int ret = 0;
        struct rv_block *nrvb;
        struct hlist_node *elem, *telem;
        int again;
        char ltype[100];

		// remove warning if DISABLE_LOGGING turned on
		(void)ltype;
		
        rv->tx_id++; /* increment transaction id */
        ASSERT(type);
        /* lower case the type */
        for (i = 0; i < 99 && type[i]; i++) {
                ltype[i] = tolower((int)type[i]);
        }
        ltype[i] = 0;
        RV_LOG_CHANGE(rv, "tx_begin, type=%s", ltype);
        ret = rv->ops.tx_start(type);
        if (ret < 0)
                goto end;
        /* link the read cache version of rvb to write cache version */
        for (i = 0; i < block_hash_size; i++) {
                hlist_for_each_entry(nrvb, elem, &rv->write_cache[i], XXhnode) {
                        nrvb->XXread = rv_find_block(rv, rv_block_get_nr(nrvb),
                                                     READ_CACHE);
                        ret = rv->ops.preprocess(nrvb);
                        if (ret < 0)
                                goto end;
                }
        }
        /* do the diff processing */
restart:
        again = 0;
        for (i = 0; i < block_hash_size; i++) {
                hlist_for_each_entry(nrvb, elem, &rv->write_cache[i], XXhnode) {
                        int done = 0;
                        if (rv_block_get_processed(nrvb))
                                continue;
                        ret = rv->ops.process(nrvb, &done);
                        if (ret < 0)
                                goto end;
                        again += done;
                }
        }
        if (again) /* it may be possible to diff some blocks again */
                goto restart;

        /* clean up write_cache */
        for (i = 0; i < block_hash_size; i++) {
                hlist_for_each_entry_safe(nrvb, elem, telem,
                                          &rv->write_cache[i], XXhnode) {
                        if (!rv_block_get_processed(nrvb)) {
                                /* probably a data block */
                                ASSERT(!nrvb->XXread);
                                rv->ops.destroy(nrvb);
                                continue;
                        }
                        if (nrvb->XXread) {
                                rv_block_put(nrvb->XXread);
                                rv->ops.destroy(nrvb->XXread);
                                nrvb->XXread = NULL;
                        }
                        /* remove nrvb from write cache */
                        hlist_del(&nrvb->XXhnode);
                        /* add nrvb to read cache */
                        /* TODO: this may require locking */
                        hlist_add_head(&nrvb->XXhnode, 
                                       &rv->read_cache[bhashfn(
                                                       rv_block_get_nr(nrvb))]);
                }
                ASSERT(hlist_empty(&rv->write_cache[i]));
        }
						
        ret = rv->ops.tx_end();
        if (ret < 0)
                goto end;
#ifdef CACHE_INVALIDATE
        ret = rv_invalidate_blocks(rv);
#endif /* CACHE_INVALIDATE */
end:
        RV_LOG_CHANGE(rv, "tx_end, type=%s\n", ltype);
        return ret;
}

/* rv_block operations */
int
rv_block_get(struct rv_block *rvb)
{
        ASSERT(rvb);
#ifdef CACHE_INVALIDATE
        ASSERT(rv_block_get_attached(rvb));
        ASSERT(!rv_block_get_on_disk(rvb));
        rvb->XXrefcount++;
#endif /* CACHE_INVALIDATE */
        return 0;
}

int
rv_block_put(struct rv_block *rvb)
{
        ASSERT(rvb);
#ifdef CACHE_INVALIDATE
        if (!rv_block_get_attached(rvb))
                return 0;
        ASSERT(!rv_block_get_on_disk(rvb));
        ASSERT(rvb->XXrefcount > 0);
        rvb->XXrefcount--;
#endif /* CACHE_INVALIDATE */
        return 0;
}

int
rv_block_get_nr(struct rv_block *rvb)
{
        return rvb->XXnr;
}

int
rv_block_get_processed(struct rv_block *rvb)
{
        return ((rvb->XXflags & RV_BLOCK_PROCESSED) != 0);
}

void
rv_block_set_processed(struct rv_block *rvb)
{
        rvb->XXflags |= RV_BLOCK_PROCESSED;
}

struct rv_block *
rv_block_get_prev_version(struct rv_block *nrvb)
{
        return nrvb->XXread;
}

static int
rv_block_get_attached(struct rv_block *rvb)
{
        return ((rvb->XXflags & RV_BLOCK_ATTACHED) != 0);
}

static void
rv_block_set_attached(struct rv_block *rvb)
{
        rvb->XXflags |= RV_BLOCK_ATTACHED;
}

#ifdef CACHE_INVALIDATE
static int
rv_block_get_on_disk(struct rv_block *rvb)
{
        return ((rvb->XXflags & RV_BLOCK_ON_DISK) != 0);
}

static void
rv_block_set_on_disk(struct rv_block *rvb, int on_disk)
{
        if (on_disk)
                rvb->XXflags |= RV_BLOCK_ON_DISK;
        else
                rvb->XXflags &= ~RV_BLOCK_ON_DISK;
}

static int
rv_block_get_being_read(struct rv_block *rvb)
{
        return ((rvb->XXflags & RV_BLOCK_BEING_READ) != 0);
}

static void
rv_block_set_being_read(struct rv_block *rvb, int being_read)
{
        if (being_read)
                rvb->XXflags |= RV_BLOCK_BEING_READ;
        else
                rvb->XXflags &= ~RV_BLOCK_BEING_READ;
}
#endif /* CACHE_INVALIDATE */

/* logging function.
 * log msg in rv.log.
 * fname is the function name.
 */
int
rv_log(const char *fname, char *msg)
{
        static FILE *fl = NULL;
        char *logfile = "rv.log";

        if (!fl) {
                if ((fl = fopen(logfile, "w")) == NULL) {
                        WARN(logfile);
                        return -errno;
                }
        }
        fprintf(fl, "%s: %s\n", fname, msg);
        fflush(fl);
        return 0;
}

/* log changes function.
 * log msg in rv_change.log.
 * fname is the function name.
 */
int
rv_log_change(struct rv *rv, const char *fname, char *msg)
{
        static FILE *fc = NULL;
        char *changefile = "rv_change.log";

        if (!fc) {
                if ((fc = fopen(changefile, "w")) == NULL) {
                        WARN(changefile);
                        return -errno;
                }
        }
        /* log change in main log file also */
        rv_log(fname, msg);
        fprintf(fc, "id=%d, %s\n", rv->tx_id, msg);
        fflush(fc);
        return 0;
}

