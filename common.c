#include "common.h"
#ifndef DISABLE_RECON
#include "rv/rv_testfs.h"
#ifndef DISABLE_PROLOG
#include <SWI-Prolog.h>
#endif
#endif
#include <sys/resource.h>
#include <sys/time.h>

static struct timeval ltod = { 0, 0 };

static void init_usage_tracker(void)
{
	int ret;
	if ( (ret = gettimeofday(&ltod, NULL)) != 0 )
	{
		EXIT("gettimeofday");
	}
}

void print_rusage(void)
{
	struct rusage usage;
	int ret = getrusage(RUSAGE_SELF, &usage);
	struct timeval tod;
	
	if ( ret != 0 )
	{
		EXIT("getrusage");
	}
	else if ( (ret = gettimeofday(&tod, NULL)) != 0 )
	{
		EXIT("gettimeofday");
	}
	else
	{
		struct timeval tdif;
		tdif.tv_usec = tod.tv_usec - ltod.tv_usec;
		tdif.tv_sec = tod.tv_sec - ltod.tv_sec;
		
		if ( tdif.tv_usec < 0 )
		{
			tdif.tv_usec += 1000000;
			tdif.tv_sec--;
		}

		printf("\nReal time: %ld.%06ld\n",
			tdif.tv_sec, tdif.tv_usec);
		printf("User time: %ld.%06ld\nSystem time: %ld.%06ld\n", 
			usage.ru_utime.tv_sec, usage.ru_utime.tv_usec, 
			usage.ru_stime.tv_sec, usage.ru_stime.tv_usec);
		printf("Page Reclaims: %ld\n", usage.ru_minflt);
		printf("Page Faults: %ld\n", usage.ru_majflt);
		printf("Block input operations: %ld\n", usage.ru_inblock);
		printf("Block output operation: %ld\n", usage.ru_oublock);
	}
}

static void init_prolog(int argc, char *argv[])
{
#ifndef DISABLE_RECON
#ifndef DISABLE_PROLOG
	static char * pl_argv[2] = { NULL, NULL };
	PL_register_extensions(rv_testfs_predicates);
	pl_argv[0] = argv[0];
	if ( !PL_initialise(1, pl_argv) )
	{
		EXIT("init_prolog");
	}
#endif
#endif
    (void)argc;
    (void)argv;
}

static void cleanup_prolog(void)
{
#ifndef DISABLE_RECON
#ifndef DISABLE_PROLOG
	PL_cleanup(0);
#endif
#endif
}

void init_modules(int argc, char *argv[])
{
        init_prolog(argc, argv);
        init_usage_tracker();
}

void cleanup_modules(void)
{
        cleanup_prolog();
}

