#include "prolog.h"
#include "rv.h"
#include <stdarg.h>
#include <string.h>

#define RV_MIN_ARITY 4
#define RV_MAX_ARITY 6
#define RV_NUM_FUNCTORS (RV_MAX_ARITY-RV_MIN_ARITY+1)

atom_t rv_super_block;
atom_t rv_inode_freemap;
atom_t rv_block_freemap;
atom_t rv_inode;
atom_t rv_indirect_block;
atom_t rv_indirect_dir_block;
atom_t rv_dir_block;
atom_t rv_action_add;
atom_t rv_action_remove;

#ifndef DISABLE_PROLOG
static functor_t rv_functor[RV_NUM_FUNCTORS] = { (functor_t)0 };

static functor_t rv_pl_get_functor(int arity)
{
	if ( arity >= RV_MIN_ARITY && arity <= RV_MAX_ARITY )
	{
		return rv_functor[arity-RV_MIN_ARITY];
	}
		
	return (functor_t)0;
}
#endif

bool rv_prolog_init(void)
{
#ifndef DISABLE_PROLOG
	int i;
	
	for ( i = 0; i < RV_NUM_FUNCTORS; i++ )
	{
		rv_functor[i] = PL_new_functor(PL_new_atom("rv"), i+RV_MIN_ARITY);
	}
	
	rv_super_block = PL_new_atom("super_block");
	rv_inode_freemap = PL_new_atom("inode_freemap");
	rv_block_freemap = PL_new_atom("block_freemap");
	rv_inode = PL_new_atom("inode");
	rv_indirect_block = PL_new_atom("indirect_block");
	rv_indirect_dir_block = PL_new_atom("indirect_dir_block");
	rv_dir_block = PL_new_atom("dir_block");
	rv_action_add = PL_new_atom("add");
	rv_action_remove = PL_new_atom("remove");
#endif
	return true;
}

bool rv_prolog_cleanup(void)
{
	// TODO: clean up required?
	return true;
}
	
bool rv_prolog_call(const char * pred, int arity)
{
#ifndef DISABLE_PROLOG
	term_t ref = PL_new_term_refs(arity);
	predicate_t p = PL_predicate(pred, arity, "user");
	qid_t qid = PL_open_query((module_t)0, PL_Q_NORMAL, p, ref);
	int rval, i;
	char * s;
	bool ret = false;
	ASSERT(qid);
	
	if ( (ret = ( rval = PL_next_solution(qid) ) != 0) )
	{
		printf("\n");
		rv_show_current_facts();
		printf("%s:\n", pred);
	}
	
	while ( rval )
	{
		printf("[");
		for ( i = 0; i < arity; i++ )
		{
			switch( PL_term_type(ref+i) )
			{
			case PL_VARIABLE:
			case PL_ATOM:
				if ( PL_get_chars(ref+i, &s, CVT_ALL) == 0 )
					printf("'%s', ", s);
				break;
			case PL_INTEGER:
			case PL_FLOAT:
				if ( PL_get_chars(ref+i, &s, CVT_ALL) )
					printf("%s, ", s);
				break;
			default:
				printf("?, ");
			}
		}
		printf("\b\b]\n");
		rval = PL_next_solution(qid);
	}	
			
	PL_cut_query(qid);
	if ( ret ) exit(0);
	return ret;
#else
	return false;
#endif
}

void rv_show_current_facts(void)
{
#ifndef DISABLE_PROLOG
	int i, j;
	for ( i = RV_MIN_ARITY; i <= RV_MAX_ARITY; i++ )
	{
		char * s;
		term_t ref = PL_new_term_refs(i);
		predicate_t p = PL_predicate("rv", i, "user");
		qid_t qid = PL_open_query((module_t)0, PL_Q_NORMAL, p, ref);
		while ( PL_next_solution(qid) )
		{
			printf("rv(");
			for ( j = 0; j < i; j++ )
			{
				switch( PL_term_type(ref+j) )
				{
				case PL_VARIABLE:
				case PL_ATOM:
					PL_ASSERT(PL_get_chars(ref+j, &s, CVT_ALL));
					printf("'%s', ", s);
					break;
				case PL_INTEGER:
				case PL_FLOAT:
					PL_ASSERT(PL_get_chars(ref+j, &s, CVT_ALL));
					printf("%s, ", s);
					break;
				default:
					printf("?, ");
				}
			}
			printf("\b\b)\n");
		}
		PL_close_query(qid);
	}
#endif
}

#ifndef DISABLE_PROLOG
static bool rv_prolog_unify_and_assert(int ac, int av)
{

	term_t ref = PL_new_term_ref();
	PL_ASSERT(PL_cons_functor_v(ref, rv_pl_get_functor(ac), av));
	predicate_t p = PL_predicate("assert", 1, "user");
	qid_t qid = PL_open_query((module_t)0, PL_Q_NORMAL, p, ref);
	int rval;
	ASSERT(qid);
	rval = PL_next_solution(qid);
	PL_cut_query(qid);
	return rval != 0;
}
#endif

bool rv_prolog_assert(const char * at, ...)
{
#ifndef DISABLE_PROLOG
	va_list ap;
	va_start(ap, at);
	int len = strlen(at);
	term_t tr = PL_new_term_refs(len);
	int i;
	
	for ( i = 0; i < len; i++ )
	{
		switch ( at[i] )
		{
		case 'a':
			PL_put_atom(tr+i, va_arg(ap, atom_t));
			break;
		case 'i':
			PL_ASSERT(PL_put_integer(tr+i, va_arg(ap, long)));
			break;
		case 's':
			PL_put_atom_chars(tr+i, va_arg(ap, const char *));
			break;
		default:
			ASSERT(false);
		}
	}
	
	return rv_prolog_unify_and_assert(len, tr);
#else
	return false;
#endif
}

bool rv_prolog_retractall(void)
{
#ifndef DISABLE_PROLOG
	term_t ref = PL_new_term_refs(0);
	predicate_t p = PL_predicate("wipe_change_log", 0, "user");
	qid_t qid = PL_open_query((module_t)0, PL_Q_NORMAL, p, ref);
	int rval;
	ASSERT(qid);
	rval = PL_next_solution(qid);
	PL_cut_query(qid);
	return rval != 0;
#else
	return false;
#endif
}
