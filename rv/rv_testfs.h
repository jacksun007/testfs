#ifndef RV_TESTFS_H
#define RV_TESTFS_H

#include "rv.h"
int rv_testfs_init(struct rv *rv, struct rv_ops **opsp, int *consistent,
                   int *multiple_updates);
                   
#ifndef DISABLE_PROLOG
#include <SWI-Prolog.h>
extern PL_extension rv_testfs_predicates[];
#endif

#endif /* RV_TESTFS_H */

