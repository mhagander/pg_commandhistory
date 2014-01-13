/*
 * pg_commandhistory.c - track a list of what's happening in a transaction
 *                       so we can dump a list of all SQL commands that have
 *                       been run since the transaction was started.
 *
 * Copyright (C) 2014 Redpill Linpro AB
 *
 * Licensed under the PostgreSQL License
 */

#include "postgres.h"
#include "funcapi.h"
#include "access/xact.h"
#include "utils/memutils.h"
#include "utils/timestamp.h"
#include "miscadmin.h"

#include <signal.h>

PG_MODULE_MAGIC;

void _PG_init(void);
static void chistory_ExecutorEnd(QueryDesc *queryDesc);
static void chistory_xact_callback(XactEvent event, void *arg);
static void usr2_handler(SIGNAL_ARGS);

static ExecutorEnd_hook_type prev_ExecutorEnd = NULL;

static List *chistory_query_stack;

/*
 * Module load callback
 */
void
_PG_init(void)
{
	chistory_query_stack = NIL;

	/*
	 * Install hooks.
	 */
	prev_ExecutorEnd = ExecutorEnd_hook;
	ExecutorEnd_hook = chistory_ExecutorEnd;

	RegisterXactCallback(chistory_xact_callback, NULL);

	/*
	 * This is somewhat evil, but we're going to hijack
	 * SIGUSR2 here. And just hope that nobody else
	 * changes it for us..
	 */
	if (pqsignal(SIGUSR2, usr2_handler) == SIG_ERR)
	{
		/*
		 * We could error, but we want to keep the general backend
		 * running even if we can't track it.
		 */
		elog(WARNING, "Failed to set USR2 signal handler: %m");
	}
}

static void
chistory_xact_callback(XactEvent event, void *arg)
{
	/*
	 * All memory is already allocated in the transaction context,
	 * but we need to reset the pointer so we start a new stack.
	 */
	chistory_query_stack = NIL;
}


static void
chistory_ExecutorEnd(QueryDesc *queryDesc)
{
	MemoryContext oldCtx;
	struct timeval tv;
	pg_time_t stamp_time;
	char *logstr;

	oldCtx = MemoryContextSwitchTo(TopTransactionContext);

	/*
	 * Store the timestamp as part of the text (!), so we leave as little
	 * work as possible for the signal handler, to be on the safe side.
	 *
	 * Need memory for the length of the query, + 8 characters for the
	 * timestamp + 1 space + the NULL byte.
	 */
	logstr = palloc(strlen(queryDesc->sourceText) + 8 + 2);

	gettimeofday(&tv, NULL);
	stamp_time = (pg_time_t) tv.tv_sec;
	pg_strftime(logstr,
				8, /* strlen("%H:%M:%S"), 2 digits each  */
				"%H:%M:%S",
				pg_localtime(&stamp_time, log_timezone));

	logstr[8] = ' ';
	strcpy(logstr+9, queryDesc->sourceText); /* adds NULL terminator */

	/*
	 * We can do lcons() on a NIL pointer in which case the list
	 * is initialized for us.
	 */
	chistory_query_stack = lcons(logstr, chistory_query_stack);

	MemoryContextSwitchTo(oldCtx);

	/*
	 * Call the next ExecutorEnd in the queue.
	 */
	if (prev_ExecutorEnd)
		prev_ExecutorEnd(queryDesc);
	else
		standard_ExecutorEnd(queryDesc);
}


static void
usr2_handler(SIGNAL_ARGS)
{
	/*
	 * Call the previous handler first, so they don't hav eto wait
	 */
	if (ImmediateInterruptOK && InterruptHoldoffCount == 0 && CritSectionCount == 0)
	{
		ListCell *lc;
		int i = 0;

		/*
		 * Hopefully it will be safe enough to elog here.. :)
		 */
		elog(LOG, "Dumping SQL history for %d", MyProcPid);
		foreach (lc, chistory_query_stack)
		{
			i++;
			elog(LOG, "%d: %s", i, (char *)lfirst(lc));
		}
		elog(LOG, "End of SQL history dump for %d", MyProcPid);
	}
}
