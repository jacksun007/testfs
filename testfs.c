#define _GNU_SOURCE
#include <stdbool.h>
#include <popt.h>
#ifndef DISABLE_RECON
#include "rv/rv_testfs.h"
#endif
#include "testfs.h"
#include "super.h"
#include "inode.h"
#include "dir.h"
#include "file.h"
#include "tx.h"

static int cmd_help(struct super_block *, struct context *c);
static int cmd_quit(struct super_block *, struct context *c);
static bool can_quit = false;

#define PROMPT printf("%s", "% ")

static struct {
	const char *name;
	int (*func)(struct super_block *sb, struct context *c);
} cmdtable[] = {
	/* menus */
	{ "?",          cmd_help },
	{ "cd",         cmd_cd },
	{ "pwd",        cmd_pwd },
	{ "ls",         cmd_ls },
	{ "lsr",        cmd_lsr },
	{ "touch",      cmd_create },
	{ "stat",       cmd_stat },
	{ "rm",         cmd_rm },
	{ "mkdir",      cmd_mkdir },
	{ "cat",        cmd_cat },
	{ "write",      cmd_write },
	{ "checkfs",    cmd_checkfs },
	{ "quit",    	cmd_quit },
        { NULL,         NULL}
};

static int
cmd_help(struct super_block *sb, struct context *c)
{
        int i = 0;
        
        printf("Commands:\n");
        for (; cmdtable[i].name; i++) {
                printf("%s\n", cmdtable[i].name);
        }
        return 0;
}

static int
cmd_quit(struct super_block *sb, struct context *c)
{
        printf("Bye!\n");
        can_quit = true;
        return 0;
}
	
void
command(struct super_block *sb, struct context *c)
{
        int i;
        if (!c->cmd[0])
                return;

	for (i=0; cmdtable[i].name; i++) {
		if (strcmp(c->cmd[0], cmdtable[i].name) == 0) {
			assert(cmdtable[i].func);
                        errno = cmdtable[i].func(sb, c);
                        if (errno < 0) {
                                errno = -errno;
                                WARN(c->cmd[0]);
                        }
                        return;
                }
        }
        printf("%s: command not found: type ?\n", c->cmd[0]);
}

static void
callback_func(poptContext context, struct poptOption *opt, char *arg,
              void *data)
{
        poptSetOtherOptionHelp(context, "[--help] [OPTION...] rawfile");
}

int
main(int argc, char *argv[])
{
	struct super_block *sb;
	char *line = NULL;
	char *token;
	size_t line_size = 0;
	ssize_t nr;
	int ret;
	struct context c;
	int corrupt = 0;
	int block_threshold = 0;
	poptContext pc; /* context for parsing command-line options */
	char ch;
	char *disk;
	struct poptOption options_table[] = {
	        {NULL, 'c', POPT_ARG_NONE, &corrupt, 'c',
	         "enable corruption", NULL},
        #ifndef DISABLE_RECON
	        {NULL, 'b', POPT_ARG_INT, &block_threshold, 'b',
	         "nr. of blocks in rv_block cache", "INT, default: unlimited"},
        #endif
	        {NULL, 0, POPT_ARG_CALLBACK | POPT_CBFLAG_PRE, callback_func},
	        POPT_AUTOHELP
	        POPT_TABLEEND
	};
	
	init_modules(argc, argv);
   /* parse command-line options */
	pc = poptGetContext(NULL, argc, (const char **)argv, options_table, 0);
	while ((ch = poptGetNextOpt(pc)) >= 0);
	if (ch < -1) { /* an error occurred during option processing */
	        poptPrintUsage(pc, stderr, 0);
	        exit(1);
	}
	/* get disk name */
	disk = (char *)poptGetArg(pc);
	if (!disk) {
	        poptPrintUsage(pc, stderr, 0);
	        exit(1);
	}
	/* no more arguments */
	if (poptGetArg(pc)) {
	        poptPrintUsage(pc, stderr, 0);
	        exit(1);
	}
	
	poptFreeContext(pc);
	printf("After Initialization:");
	print_rusage();
	
	ret = testfs_init_super_block(disk, corrupt, block_threshold, &sb);
	if (ret) {
		EXIT("testfs_init_super_block");
	}
	c.cur_dir = testfs_get_inode(sb, 0); /* root dir */
	for (;
#ifndef DISABLE_PROMPT		
		PROMPT, 
#endif
		(nr = getline(&line, &line_size, stdin)) != EOF; ) {
		token = line;
		c.nargs = 0;
		bzero(c.cmd, sizeof(char *) * MAX_ARGS);
		while ((token = strtok(token, " \t\n")) != NULL) {
				if (c.nargs < MAX_ARGS)
						c.cmd[c.nargs++] = token;
				token = NULL;
		}
		command(sb, &c);
	        if ( can_quit ) break;
	}
	testfs_put_inode(c.cur_dir);
	testfs_close_super_block(sb);
	cleanup_modules();
	printf("\nJust Before Exit:");
	print_rusage();
        exit(0);
}
