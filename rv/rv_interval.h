#ifndef _RV_BLOCK_INTERVAL_H
#define _RV_BLOCK_INTERVAL_H

struct rv_interval;

int rv_interval_create(struct rb_root *root, int start, int end, int type, 
                       struct rv_interval **rvip);
void rv_interval_delete(struct rb_root *root, struct rv_interval *rvi);
void rv_interval_destroy(struct rb_root *root);
struct rv_interval *rv_interval_find(struct rb_root *root, int start);
int rv_interval_type(struct rv_interval *rvi);
#endif /* _RV_BLOCK_INTERVAL_H */
