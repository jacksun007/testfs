% TODO: 
% Rule 7 and 8 will not work if the same block may be allocated
% and deallocated multiple times within a transaction
% =============================================================
% list of facts and primitives

% rv(inode, INODE_NR, OFFSET, OLD, NEW)
% where OFFSET is one of:
% 0: inode type (either 0 for NONE, 1 for FILE or 2 for DIR)
% 1: modification time
% 2: size
% 3-6: direct blocks
% 7: indirect block

% rv(block_freemap, BLOCK_NR, OLD, NEW)
% rv(inode_freemap, INODE_NR, OLD, NEW)

% rv(dir_block, ACTION, DIR_INODE_NR, NAME, INODE_NR, DIRENT_SIZE)
% where ACTION is one of: add, remove.
% rv(indirect_dir_block, BLOCK_NR, INDEX, OLD, NEW)

% rv(super_block, OFFSET, OLD, NEW)
% where OFFSET is one of:
% 0: inode_freemap_start
% 1: block_freemap_start
% 2: inode_blocks_start
% 3: data_blocks_start
% 4: modification_time

% dir_get_child(DIN, NAME, IN)
% DIN [in]: dir_inode_nr
% NAME [in]: name of child directory
% IN [out]: inode number.
% returns true if found, false otherwise

% inode_get(IN, FIELD, OUT)
% IN [in]: inode number
% FIELD [in]: field offset of the inode
% OUT [out]: the value of the field in the cache
% returns true if inode is within bound, else false
% =============================================================

:- use_module(library(aggregate)).
:- dynamic rv/4.
:- dynamic rv/5.
:- dynamic rv/6.

wipe_change_log :- retractall(rv(_,_,_,_)),
	retractall(rv(_,_,_,_,_)),
	retractall(rv(_,_,_,_,_,_)).

% useful predicates
block_allocated(IP, IC) :- rv(inode, IP, FIELD, 0, IC),
        FIELD >= 3, FIELD =< 7.
block_allocated(IP, IC) :- (TYPE = indirect_dir_block; TYPE = indirect_block),
		rv(TYPE, IP, IDX, 0, IC), IDX >= 0, IDX < 16.
block_deallocated(IP, IC) :- rv(inode, IP, FIELD, IC, NEW), NEW \= IC, IC \= 0,
        FIELD >= 3, FIELD =< 7.
block_deallocated(IP, IC) :- (TYPE = indirect_dir_block; TYPE = indirect_block),
		rv(TYPE, IP, IDX, IC, NEW), NEW \= IC, IC \= 0, IDX >= 0, IDX < 16.		

% TestFS Specific

% RULE B: for all indirect block allocation, the indices must be
% between 0 and 15
rule_B_fail(IP, IC, IDX) :- rv(T, IP, IDX, 0, IC), 
	(T = indirect_block; T = indirect_dir_block),
	(IDX < 0; IDX >= 16).
rule_B_fail(IP, IC, IDX) :- rv(T, IP, IDX, IC, 0), 
	(T = indirect_block; T = indirect_dir_block),
	(IDX < 0; IDX >= 16).

% RULE C: for all inode allocation, the file type must be either
% file (1) or directory (2)
rule_C_fail(ID, TYPE) :- rv(inode, ID, 0, 0, TYPE), (TYPE < 1; TYPE > 2).
rule_C_fail(ID, TYPE) :- rv(inode, ID, 0, TYPE, 0), (TYPE < 1; TYPE > 2).

% RULE 6: (blockptr, new = X) => count(blkptr, new = X) = 1;
rule_6_fail(BN) :- aggregate(count, X, block_allocated(X, BN), C), C > 1.
	
% RULE 7: (blockptr, new = X) => (BBM, id=X, new = 1) || (blkptr, old=X)
rule_7_fail(IP, IC) :- block_allocated(IP, IC), 
	(rv(block_freemap, IC, _, 0); not(rv(block_freemap, IC, _, 1))).
	
% RULE 8: (blockptr, old = X) => (BBM, id=X, new = 0) || (blkptr, new=X)
rule_8_fail(IP, IC) :- block_deallocated(IP, IC), 
	(rv(block_freemap, IC, _, 1); not(rv(block_freemap, IC, _, 0))).
	
% RULE 9: (BBM, id = X, new = 1) => (blkptr, new = X)
rule_9_fail(BN) :- rv(block_freemap, BN, 0, 1), 
	not(block_allocated(_, BN)).

% RULE 10: (BBM, id = X, new = 0) => (blkptr, old = X)
rule_10_fail(BN) :- rv(block_freemap, BN, 1, 0), 
	not(block_deallocated(_, BN)).
		
% RULE 20 (dir, id=Z, new.dotdot = X) <=> (dir, id = X, newentry.inode = Z && newentry.filetype = 2)
% RULE 21 (dir, id=Z, old.dotdot = X) <=> (dir, id = X, lostentry.inode = Z)
% NOTE: need to make sure that we're dealing with directory allocation only
rule_20_or_21_fail(A, P, C, N) :- rv(dir_block, A, P, N, C, _),
	(rv(inode, C, 0, 0, 2); rv(inode, C, 0, 2, 0)),
	N \= '.', N \= '..',
	not(rv(dir_block, A, C, '..', P, 11)).
rule_20_or_21_fail(A, P, C) :- rv(dir_block, A, C, '..', P, 11),
	(rv(inode, C, 0, 0, 2); rv(inode, C, 0, 2, 0)),
	not(rv(dir_block, A, P, _, C, _)).
	
% RULE 23: Directory can reach root through ..

get_block_type(IN, TYPE) :- rv(inode, IN, 0, _, TYPE).
get_block_type(IN, TYPE) :- inode_get(IN, 0, TYPE).
dir_get_parent(IC, IP) :- rv(dir_block, add, IC, '..', IP, _). 
dir_get_parent(IC, IP) :- dir_get_child(IC, '..', IP).

% if recursion is found, return true (cannot reach root for sure)
no_root(C, T) :- member(C, T).

% if this node has a child that is not the root node, go deeper
no_root(C, T) :- not(member(C, T)),
	dir_get_parent(C, P), P \= 0,  no_root(P, [C|T]).

rule_23_fail(DIN, N) :- ( rv(dir_block, add, _, N, DIN, _); 
	rv(dir_block, remove, DIN, N, _, _) ),
	N \= '.', N \= '..', DIN \= 0,
	get_block_type(DIN, TYPE), TYPE =:= 2,
	no_root(DIN, []).

% RULE 25: superblock write time not in future
rule_25_fail :- get_time(T), rv(super_block, 4, _, NEW), NEW =< T.

% Rule 32: Directory block(sector) count < 2^(21-log block size), File block
% (sector) count < 2%(31-log block size, which isn't actually log block size)
get_indirect_block(IN, IND) :- rv(inode, IN, 7, 0, IND).
get_indirect_block(IN, IND) :- inode_get(IN, 7, IND).

num_blocks_required(OLD, NEW, NB) :-
	NEWB is ceiling(NEW/64),
	OLDB is ceiling(OLD/64),
	NB is NEWB - OLDB.

direct_block_allocated(IP, IC) :- rv(inode, IP, FIELD, 0, IC),
        FIELD >= 3, FIELD =< 6.
indirect_block_allocated(IP, IC) :- rv(indirect_block, IP, IDX, 0, IC), IDX >= 0, IDX < 16.
direct_block_deallocated(IP, IC) :- rv(inode, IP, FIELD, IC, NEW), NEW \= IC,
        FIELD >= 3, FIELD =< 6.
indirect_block_deallocated(IP, IC) :- rv(indirect_block, IP, IDX, IC, NEW), NEW \= IC, IDX >= 0, IDX < 16.		

% the first half the rule checks only for cases where allocation is required
% NS - new size
% DC - count of direct blocks allocated
% IC - count of indirect blocks allocated
rule_32_fail(IN, NR_BLKS, NB) :- rv(inode, IN, 2, OS, NS), 
	num_blocks_required(OS, NS, NB), NB > 0,
	aggregate(count, X, direct_block_allocated(IN, X), DC),
	get_indirect_block(IN, IND), 
	aggregate(count, X, indirect_block_allocated(IND, X), IC),
	NR_BLKS is DC + IC, NB \= NR_BLKS.
	
% the second half the rule checks only for cases where deallocation is required
% NS - new size
% DC - count of direct blocks allocated
% IC - count of indirect blocks allocated
rule_32_fail(IN, NR_BLKS, NB) :- rv(inode, IN, 2, OS, NS), 
	num_blocks_required(OS, NS, A), A < 0, NB is -A,
	aggregate(count, X, direct_block_deallocated(IN, X), DC),
	get_indirect_block(IN, IND), 
	aggregate(count, X, indirect_block_deallocated(IND, X), IC),
	NR_BLKS is DC + IC, NB \= NR_BLKS. 

% RULE 36: Root inode bit immutable
rule_36_fail :- rv(inode_freemap, 0, _, 0).

