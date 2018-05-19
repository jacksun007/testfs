#include "rv.h"
#include "../list.h"
#include "rbtree.h"

struct rv_interval {
        struct rb_node rbnode;
        int start;
        int end;
        int type;
};

/*
 * Interval tree functions 
 */

/* Returns negative value on error */
int
rv_interval_create(struct rb_root *root, int start, int end, int type,
                   struct rv_interval **rvip)
{
        struct rb_node **link = &root->rb_node;
        struct rb_node *parent = NULL;
        struct rv_interval *rvi;

        ASSERT(start <= end);
        /* Go to the bottom of the tree */
        while (*link)
        {
                parent = *link;
                rvi = rb_entry(parent, struct rv_interval, rbnode);
                if (end <= rvi->start)
                        link = &(*link)->rb_left;
                else if (start >= rvi->end)
                        link = &(*link)->rb_right;
                else /* overlapping interval */
                        RET_ERROR(-EINVAL);
        }
        RET_NOMEM(rvi = malloc(sizeof(struct rv_interval)));
        rvi->start = start;
        rvi->end = end;
        rvi->type = type;
        /* Put the new node there */
        rb_link_node(&rvi->rbnode, parent, link);
        rb_insert_color(&rvi->rbnode, root);
        *rvip = rvi;
        return 0;
}

void
rv_interval_delete(struct rb_root *root, struct rv_interval *rvi)
{
        rb_erase(&rvi->rbnode, root);
        free(rvi);
}

/* delete all intervals */
void
rv_interval_destroy(struct rb_root *root)
{
        struct rb_node *n;
        struct rv_interval *rvi;

        while ((n = root->rb_node)!= NULL) {
                rvi = rb_entry(n, struct rv_interval, rbnode);
                rv_interval_delete(root, rvi);
        }
}

struct rv_interval *
rv_interval_find(struct rb_root *root, int nr)
{
        struct rb_node *n = root->rb_node; /* top of the tree */
        struct rv_interval *rvi;

        while (n) {
                rvi = rb_entry(n, struct rv_interval, rbnode);

                if (nr < rvi->start)
                        n = n->rb_left;
                else if (nr >= rvi->end)
                        n = n->rb_right;
                else
                        return rvi;
        }
        return NULL;
}

int
rv_interval_type(struct rv_interval *rvi)
{
        return rvi->type;
}

