#ifndef _PROLOG_H
#define _PROLOG_H

#include <SWI-Prolog.h>
#include <stdbool.h>

bool rv_prolog_init(void);
bool rv_prolog_cleaup(void);
void rv_show_current_facts(void);
bool rv_prolog_assert(const char * at, ...);
bool rv_prolog_call(const char * pred, int arity);
bool rv_prolog_retractall(void);

extern atom_t rv_super_block;
extern atom_t rv_inode_freemap;
extern atom_t rv_block_freemap;
extern atom_t rv_inode;
extern atom_t rv_indirect_block;
extern atom_t rv_indirect_dir_block;
extern atom_t rv_dir_block;
extern atom_t rv_action_add;
extern atom_t rv_action_remove;

#define PL_ASSERT(expr) \
	do { \
		if ((expr) != 0) { \
			RV_LOG(#expr "failed"); \
		} \
	} while(0) 

#endif

