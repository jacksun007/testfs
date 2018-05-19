#include "super.h"
#include "tx.h"
#include "rv/rv.h"

char *tx_type_array[] = {"TX_NONE",
                         "TX_WRITE",
                         "TX_CREATE",
                         "TX_RM",
                         "TX_UMOUNT"};

void
testfs_tx_start(struct super_block *sb, tx_type type)
{
        assert(sb->tx_in_progress == TX_NONE);
        sb->tx_in_progress = type;
#ifndef DISABLE_RECON
        rv_tx_start(sb->rv, tx_type_array[type]);
#endif
}

void
testfs_tx_commit(struct super_block *sb, tx_type type)
{
        assert(sb->tx_in_progress == type);
        sb->tx_in_progress = TX_NONE;
#ifndef DISABLE_RECON
        rv_tx_commit(sb->rv, tx_type_array[type]);
#endif
}
