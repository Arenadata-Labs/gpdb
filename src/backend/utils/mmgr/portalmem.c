/*-------------------------------------------------------------------------
 *
 * portalmem.c
 *	  backend portal memory management
 *
 * Portals are objects representing the execution state of a query.
 * This module provides memory management services for portals, but it
 * doesn't actually run the executor for them.
 *
 *
 * Portions Copyright (c) 2006-2009, Greenplum inc
 * Portions Copyright (c) 2012-Present Pivotal Software, Inc.
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/utils/mmgr/portalmem.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/pg_type.h"
#include "commands/portalcmds.h"
#include "miscadmin.h"
#include "utils/builtins.h"
#include "utils/memutils.h"
#include "utils/resource_manager.h"
#include "utils/resscheduler.h"

#include "cdb/ml_ipc.h"
#include "utils/timestamp.h"

/*
 * Estimate of the maximum number of open portals a user would have,
 * used in initially sizing the PortalHashTable in EnablePortalManager().
 * Since the hash table can expand, there's no need to make this overly
 * generous, and keeping it small avoids unnecessary overhead in the
 * hash_seq_search() calls executed during transaction end.
 */
#define PORTALS_PER_USER	   16


/* ----------------
 *		Global state
 * ----------------
 */

#define MAX_PORTALNAME_LEN		NAMEDATALEN

typedef struct portalhashent
{
	char		portalname[MAX_PORTALNAME_LEN];
	Portal		portal;
} PortalHashEnt;

static HTAB *PortalHashTable = NULL;

#define PortalHashTableLookup(NAME, PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_FIND, NULL); \
	if (hentry) \
		PORTAL = hentry->portal; \
	else \
		PORTAL = NULL; \
} while(0)

#define PortalHashTableInsert(PORTAL, NAME) \
do { \
	PortalHashEnt *hentry; bool found; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   (NAME), HASH_ENTER, &found); \
	if (found) \
		elog(ERROR, "duplicate portal name"); \
	hentry->portal = PORTAL; \
	/* To avoid duplicate storage, make PORTAL->name point to htab entry */ \
	PORTAL->name = hentry->portalname; \
} while(0)

#define PortalHashTableDelete(PORTAL) \
do { \
	PortalHashEnt *hentry; \
	\
	hentry = (PortalHashEnt *) hash_search(PortalHashTable, \
										   PORTAL->name, HASH_REMOVE, NULL); \
	if (hentry == NULL) \
		elog(WARNING, "trying to delete portal name that does not exist"); \
} while(0)

static MemoryContext PortalMemory = NULL;


/* ----------------------------------------------------------------
 *				   public portal interface functions
 * ----------------------------------------------------------------
 */

/*
 * EnablePortalManager
 *		Enables the portal management module at backend startup.
 */
void
EnablePortalManager(void)
{
	HASHCTL		ctl;

	Assert(PortalMemory == NULL);

	PortalMemory = AllocSetContextCreate(TopMemoryContext,
										 "PortalMemory",
										 ALLOCSET_DEFAULT_MINSIZE,
										 ALLOCSET_DEFAULT_INITSIZE,
										 ALLOCSET_DEFAULT_MAXSIZE);

	ctl.keysize = MAX_PORTALNAME_LEN;
	ctl.entrysize = sizeof(PortalHashEnt);

	/*
	 * use PORTALS_PER_USER as a guess of how many hash table entries to
	 * create, initially
	 */
	PortalHashTable = hash_create("Portal hash", PORTALS_PER_USER,
								  &ctl, HASH_ELEM);
}

/*
 * GetPortalByName
 *		Returns a portal given a portal name, or NULL if name not found.
 */
Portal
GetPortalByName(const char *name)
{
	Portal		portal;

	if (PointerIsValid(name))
		PortalHashTableLookup(name, portal);
	else
		portal = NULL;

	return portal;
}

/*
 * PortalListGetPrimaryStmt
 *		Get the "primary" stmt within a portal, ie, the one marked canSetTag.
 *
 * Returns NULL if no such stmt.  If multiple PlannedStmt structs within the
 * portal are marked canSetTag, returns the first one.  Neither of these
 * cases should occur in present usages of this function.
 *
 * Copes if given a list of Querys --- can't happen in a portal, but this
 * code also supports plancache.c, which needs both cases.
 *
 * Note: the reason this is just handed a List is so that plancache.c
 * can share the code.  For use with a portal, use PortalGetPrimaryStmt
 * rather than calling this directly.
 */
Node *
PortalListGetPrimaryStmt(List *stmts)
{
	ListCell   *lc;

	foreach(lc, stmts)
	{
		Node	   *stmt = (Node *) lfirst(lc);

		if (IsA(stmt, PlannedStmt))
		{
			if (((PlannedStmt *) stmt)->canSetTag)
				return stmt;
		}
		else if (IsA(stmt, Query))
		{
			if (((Query *) stmt)->canSetTag)
				return stmt;
		}
		else
		{
			/* Utility stmts are assumed canSetTag if they're the only stmt */
			if (list_length(stmts) == 1)
				return stmt;
		}
	}
	return NULL;
}

/*
 * CreatePortal
 *		Returns a new portal given a name.
 *
 * allowDup: if true, automatically drop any pre-existing portal of the
 * same name (if false, an error is raised).
 *
 * dupSilent: if true, don't even emit a WARNING.
 */
Portal
CreatePortal(const char *name, bool allowDup, bool dupSilent)
{
	Portal		portal;

	AssertArg(PointerIsValid(name));

	portal = GetPortalByName(name);
	if (PortalIsValid(portal))
	{
		if (!allowDup)
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("cursor \"%s\" already exists", name)));
		if (!dupSilent && Gp_role != GP_ROLE_EXECUTE)
			ereport(WARNING,
					(errcode(ERRCODE_DUPLICATE_CURSOR),
					 errmsg("closing existing cursor \"%s\"",
							name)));
		PortalDrop(portal, false);
	}

	/* make new portal structure */
	portal = (Portal) MemoryContextAllocZero(PortalMemory, sizeof *portal);

	/* initialize portal heap context; typically it won't store much */
	portal->heap = AllocSetContextCreate(PortalMemory,
										 "PortalHeapMemory",
										 ALLOCSET_SMALL_MINSIZE,
										 ALLOCSET_SMALL_INITSIZE,
										 ALLOCSET_SMALL_MAXSIZE);

	/* create a resource owner for the portal */
	portal->resowner = ResourceOwnerCreate(CurTransactionResourceOwner,
										   "Portal");

	/* initialize portal fields that don't start off zero */
	portal->status = PORTAL_NEW;
	portal->cleanup = PortalCleanup;
	portal->createSubid = GetCurrentSubTransactionId();
	portal->activeSubid = portal->createSubid;
	portal->strategy = PORTAL_MULTI_QUERY;
	portal->cursorOptions = CURSOR_OPT_NO_SCROLL;
	portal->atStart = true;
	portal->atEnd = true;		/* disallow fetches until query is set */
	portal->visible = true;
	portal->creation_time = GetCurrentStatementStartTimestamp();

	if (IsResQueueEnabled())
	{
		/* Only QD needs to set portal id if have enabled resource scheduling */
		if (Gp_role == GP_ROLE_DISPATCH)
		{
			portal->portalId = ResCreatePortalId(name);
			portal->queueId = GetResQueueId();
		}
		else if (Gp_role == GP_ROLE_EXECUTE)
			portal->queueId = GetResQueueId();
	}
	portal->is_extended_query = false; /* default value */

	/* put portal in table (sets portal->name) */
	PortalHashTableInsert(portal, name);

	/* Setup gpmon. Siva - should this be moved elsewhere? */
	gpmon_init();

	/* End Gpmon */

	return portal;
}

/*
 * CreateNewPortal
 *		Create a new portal, assigning it a random nonconflicting name.
 */
Portal
CreateNewPortal(void)
{
	static unsigned int unnamed_portal_count = 0;

	char		portalname[MAX_PORTALNAME_LEN];

	/* Select a nonconflicting name */
	for (;;)
	{
		unnamed_portal_count++;
		sprintf(portalname, "<unnamed portal %u>", unnamed_portal_count);
		if (GetPortalByName(portalname) == NULL)
			break;
	}

	return CreatePortal(portalname, false, false);
}

/*
 * PortalDefineQuery
 *		A simple subroutine to establish a portal's query.
 *
 * Notes: as of PG 8.4 (this part backported to GPDB already), caller MUST supply a sourceText string; it is not
 * allowed anymore to pass NULL.  (If you really don't have source text,
 * you can pass a constant string, perhaps "(query not available)".)
 *
 * commandTag shall be NULL if and only if the original query string
 * (before rewriting) was an empty string.  Also, the passed commandTag must
 * be a pointer to a constant string, since it is not copied.
 *
 * If cplan is provided, then it is a cached plan containing the stmts, and
 * the caller must have done GetCachedPlan(), causing a refcount increment.
 * The refcount will be released when the portal is destroyed.
 *
 * If cplan is NULL, then it is the caller's responsibility to ensure that
 * the passed plan trees have adequate lifetime.  Typically this is done by
 * copying them into the portal's heap context.
 *
 * The caller is also responsible for ensuring that the passed prepStmtName
 * (if not NULL) and sourceText have adequate lifetime.
 *
 * NB: this function mustn't do much beyond storing the passed values; in
 * particular don't do anything that risks elog(ERROR).  If that were to
 * happen here before storing the cplan reference, we'd leak the plancache
 * refcount that the caller is trying to hand off to us.
 */
void
PortalDefineQuery(Portal portal,
				  const char *prepStmtName,
				  const char *sourceText,
				  NodeTag	  sourceTag,
				  const char *commandTag,
				  List *stmts,
				  CachedPlan *cplan)
{
	AssertArg(PortalIsValid(portal));
	AssertState(portal->status == PORTAL_NEW);

	AssertArg(sourceText != NULL);
	AssertArg(commandTag != NULL || stmts == NIL);

	portal->prepStmtName = prepStmtName;
	portal->sourceText = sourceText;
	portal->sourceTag = sourceTag;
	portal->commandTag = commandTag;
	portal->stmts = stmts;
	portal->cplan = cplan;
	portal->status = PORTAL_DEFINED;
}

/*
 * PortalReleaseCachedPlan
 *		Release a portal's reference to its cached plan, if any.
 */
static void
PortalReleaseCachedPlan(Portal portal)
{
	if (portal->cplan)
	{
		ReleaseCachedPlan(portal->cplan, false);
		portal->cplan = NULL;

		/*
		 * We must also clear portal->stmts which is now a dangling reference
		 * to the cached plan's plan list.  This protects any code that might
		 * try to examine the Portal later.
		 */
		portal->stmts = NIL;
	}
}

/*
 * PortalCreateHoldStore
 *		Create the tuplestore for a portal.
 */
void
PortalCreateHoldStore(Portal portal)
{
	MemoryContext oldcxt;

	Assert(portal->holdContext == NULL);
	Assert(portal->holdStore == NULL);

	/*
	 * Create the memory context that is used for storage of the tuple set.
	 * Note this is NOT a child of the portal's heap memory.
	 */
	portal->holdContext =
		AllocSetContextCreate(PortalMemory,
							  "PortalHoldContext",
							  ALLOCSET_DEFAULT_MINSIZE,
							  ALLOCSET_DEFAULT_INITSIZE,
							  ALLOCSET_DEFAULT_MAXSIZE);

	/*
	 * Create the tuple store, selecting cross-transaction temp files, and
	 * enabling random access only if cursor requires scrolling.
	 *
	 * XXX: Should maintenance_work_mem be used for the portal size?
	 */
	oldcxt = MemoryContextSwitchTo(portal->holdContext);

	portal->holdStore =
		tuplestore_begin_heap(portal->cursorOptions & CURSOR_OPT_SCROLL,
							  true, work_mem);

	MemoryContextSwitchTo(oldcxt);
}

/*
 * PinPortal
 *		Protect a portal from dropping.
 *
 * A pinned portal is still unpinned and dropped at transaction or
 * subtransaction abort.
 */
void
PinPortal(Portal portal)
{
	if (portal->portalPinned)
		elog(ERROR, "portal already pinned");

	portal->portalPinned = true;
}

void
UnpinPortal(Portal portal)
{
	if (!portal->portalPinned)
		elog(ERROR, "portal not pinned");

	portal->portalPinned = false;
}

/*
 * MarkPortalActive
 *		Transition a portal from READY to ACTIVE state.
 *
 * NOTE: never set portal->status = PORTAL_ACTIVE directly; call this instead.
 */
void
MarkPortalActive(Portal portal)
{
	/* For safety, this is a runtime test not just an Assert */
	if (portal->status != PORTAL_READY)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("portal \"%s\" cannot be run", portal->name)));
	/* Perform the state transition */
	portal->status = PORTAL_ACTIVE;
	portal->activeSubid = GetCurrentSubTransactionId();
}

/*
 * MarkPortalDone
 *		Transition a portal from ACTIVE to DONE state.
 *
 * NOTE: never set portal->status = PORTAL_DONE directly; call this instead.
 */
void
MarkPortalDone(Portal portal)
{
	/* Perform the state transition */
	Assert(portal->status == PORTAL_ACTIVE);
	portal->status = PORTAL_DONE;

	/*
	 * Allow portalcmds.c to clean up the state it knows about.  We might as
	 * well do that now, since the portal can't be executed any more.
	 *
	 * In some cases involving execution of a ROLLBACK command in an already
	 * aborted transaction, this is necessary, or we'd reach AtCleanup_Portals
	 * with the cleanup hook still unexecuted.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		(*portal->cleanup) (portal);
		portal->cleanup = NULL;
	}
}

/*
 * MarkPortalFailed
 *		Transition a portal into FAILED state.
 *
 * NOTE: never set portal->status = PORTAL_FAILED directly; call this instead.
 */
void
MarkPortalFailed(Portal portal)
{
	/* Perform the state transition */
	Assert(portal->status != PORTAL_DONE);
	portal->status = PORTAL_FAILED;

	/*
	 * Allow portalcmds.c to clean up the state it knows about.  We might as
	 * well do that now, since the portal can't be executed any more.
	 *
	 * In some cases involving cleanup of an already aborted transaction, this
	 * is necessary, or we'd reach AtCleanup_Portals with the cleanup hook
	 * still unexecuted.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		(*portal->cleanup) (portal);
		portal->cleanup = NULL;
	}
}

/*
 * PortalDrop
 *		Destroy the portal.
 */
void
PortalDrop(Portal portal, bool isTopCommit)
{
	AssertArg(PortalIsValid(portal));

	/*
	 * Don't allow dropping a pinned portal, it's still needed by whoever
	 * pinned it. Not sure if the PORTAL_ACTIVE case can validly happen or
	 * not...
	 */
	if (portal->portalPinned ||
		portal->status == PORTAL_ACTIVE)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_CURSOR_STATE),
				 errmsg("cannot drop active portal \"%s\"", portal->name)));

	/*
	 * Allow portalcmds.c to clean up the state it knows about, in particular
	 * shutting down the executor if still active.  This step potentially runs
	 * user-defined code so failure has to be expected.  It's the cleanup
	 * hook's responsibility to not try to do that more than once, in the case
	 * that failure occurs and then we come back to drop the portal again
	 * during transaction abort.
	 *
	 * Note: in most paths of control, this will have been done already in
	 * MarkPortalDone or MarkPortalFailed.  We're just making sure.
	 */
	if (PointerIsValid(portal->cleanup))
	{
		(*portal->cleanup) (portal);
		portal->cleanup = NULL;
	}

	/*
	 * Remove portal from hash table.  Because we do this here, we will not
	 * come back to try to remove the portal again if there's any error in the
	 * subsequent steps.  Better to leak a little memory than to get into an
	 * infinite error-recovery loop.
	 */
	PortalHashTableDelete(portal);

	if (IsResQueueLockedForPortal(portal))
	{
		ResUnLockPortal(portal);
	}

	/* drop cached plan reference, if any */
	PortalReleaseCachedPlan(portal);

	/*
	 * Release any resources still attached to the portal.  There are several
	 * cases being covered here:
	 *
	 * Top transaction commit (indicated by isTopCommit): normally we should
	 * do nothing here and let the regular end-of-transaction resource
	 * releasing mechanism handle these resources too.  However, if we have a
	 * FAILED portal (eg, a cursor that got an error), we'd better clean up
	 * its resources to avoid resource-leakage warning messages.
	 *
	 * Sub transaction commit: never comes here at all, since we don't kill
	 * any portals in AtSubCommit_Portals().
	 *
	 * Main or sub transaction abort: we will do nothing here because
	 * portal->resowner was already set NULL; the resources were already
	 * cleaned up in transaction abort.
	 *
	 * Ordinary portal drop: must release resources.  However, if the portal
	 * is not FAILED then we do not release its locks.  The locks become the
	 * responsibility of the transaction's ResourceOwner (since it is the
	 * parent of the portal's owner) and will be released when the transaction
	 * eventually ends.
	 */
	if (portal->resowner &&
		(!isTopCommit || portal->status == PORTAL_FAILED))
	{
		bool		isCommit = (portal->status != PORTAL_FAILED);

		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_BEFORE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_LOCKS,
							 isCommit, false);
		ResourceOwnerRelease(portal->resowner,
							 RESOURCE_RELEASE_AFTER_LOCKS,
							 isCommit, false);
		ResourceOwnerDelete(portal->resowner);
	}
	portal->resowner = NULL;

	/*
	 * Delete tuplestore if present.  We should do this even under error
	 * conditions; since the tuplestore would have been using cross-
	 * transaction storage, its temp files need to be explicitly deleted.
	 */
	if (portal->holdStore)
	{
		MemoryContext oldcontext;

		oldcontext = MemoryContextSwitchTo(portal->holdContext);
		tuplestore_end(portal->holdStore);
		MemoryContextSwitchTo(oldcontext);
		portal->holdStore = NULL;
	}

	/* delete tuplestore storage, if any */
	if (portal->holdContext)
		MemoryContextDelete(portal->holdContext);

	/* release subsidiary storage */
	MemoryContextDelete(PortalGetHeapMemory(portal));

	/* release portal struct (it's in PortalMemory) */
	pfree(portal);
}

/*
 * Delete all declared cursors.
 *
 * Used by commands: CLOSE ALL, DISCARD ALL
 */
void
PortalHashTableDeleteAll(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	if (PortalHashTable == NULL)
		return;

	hash_seq_init(&status, PortalHashTable);
	while ((hentry = hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Can't close the active portal (the one running the command) */
		if (portal->status == PORTAL_ACTIVE)
			continue;

		PortalDrop(portal, false);

		/* Restart the iteration in case that led to other drops */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}
}


/*
 * Pre-commit processing for portals.
 *
 * Holdable cursors created in this transaction need to be converted to
 * materialized form, since we are going to close down the executor and
 * release locks.  Non-holdable portals created in this transaction are
 * simply removed.  Portals remaining from prior transactions should be
 * left untouched.
 *
 * Returns TRUE if any portals changed state (possibly causing user-defined
 * code to be run), FALSE if not.
 */
bool
PreCommit_Portals(bool isPrepare)
{
	bool		result = false;
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/*
		 * There should be no pinned portals anymore. Complain if someone
		 * leaked one.
		 */
		if (portal->portalPinned)
			elog(ERROR, "cannot commit while a portal is pinned");

		/*
		 * Do not touch active portals --- this can only happen in the case of
		 * a multi-transaction utility command, such as VACUUM.
		 *
		 * Note however that any resource owner attached to such a portal is
		 * still going to go away, so don't leave a dangling pointer.
		 */
		if (portal->status == PORTAL_ACTIVE)
		{
			portal->resowner = NULL;
			continue;
		}

		/* Is it a holdable portal created in the current xact? */
		if ((portal->cursorOptions & CURSOR_OPT_HOLD) &&
			portal->createSubid != InvalidSubTransactionId &&
			portal->status == PORTAL_READY)
		{
			/*
			 * We are exiting the transaction that created a holdable cursor.
			 * Instead of dropping the portal, prepare it for access by later
			 * transactions.
			 *
			 * However, if this is PREPARE TRANSACTION rather than COMMIT,
			 * refuse PREPARE, because the semantics seem pretty unclear.
			 */
			if (isPrepare)
				ereport(ERROR,
						(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						 errmsg("cannot PREPARE a transaction that has created a cursor WITH HOLD")));

			/*
			 * Note that PersistHoldablePortal() must release all resources
			 * used by the portal that are local to the creating transaction.
			 */
			PortalCreateHoldStore(portal);
			PersistHoldablePortal(portal);

			/* drop cached plan reference, if any */
			PortalReleaseCachedPlan(portal);

			/*
			 * Any resources belonging to the portal will be released in the
			 * upcoming transaction-wide cleanup; the portal will no longer
			 * have its own resources.
			 */
			portal->resowner = NULL;

			/*
			 * Having successfully exported the holdable cursor, mark it as
			 * not belonging to this transaction.
			 */
			portal->createSubid = InvalidSubTransactionId;
			portal->activeSubid = InvalidSubTransactionId;

			/* Report we changed state */
			result = true;
		}
		else if (portal->createSubid == InvalidSubTransactionId)
		{
			/*
			 * Do nothing to cursors held over from a previous transaction
			 * (including ones we just froze in a previous cycle of this loop)
			 */
			continue;
		}
		else
		{
			/* Zap all non-holdable portals */
			PortalDrop(portal, true);

			/* Report we changed state */
			result = true;
		}

		/*
		 * After either freezing or dropping a portal, we have to restart the
		 * iteration, because we could have invoked user-defined code that
		 * caused a drop of the next portal in the hash chain.
		 */
		hash_seq_term(&status);
		hash_seq_init(&status, PortalHashTable);
	}

	return result;
}

/*
 * Abort processing for portals.
 *
 * At this point we reset "active" status and run the cleanup hook if
 * present, but we can't release the portal's memory until the cleanup call.
 *
 * The reason we need to reset active is so that we can replace the unnamed
 * portal, else we'll fail to execute ROLLBACK when it arrives.
 */
void
AtAbort_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Any portal that was actually running has to be considered broken */
		if (portal->status == PORTAL_ACTIVE)
			MarkPortalFailed(portal);

		if (portal->is_extended_query && portal->queryDesc != NULL)
		{
			Assert(portal->queryDesc->estate != NULL);
			portal->queryDesc->estate->cancelUnfinished = true;
		}

		/*
		 * Do nothing else to cursors held over from a previous transaction.
		 */
		if (portal->createSubid == InvalidSubTransactionId)
			continue;

		/*
		 * GPDB_90_MERGE_FIXME: This was added in commit 7981c342, to prevent
		 * ExecutorEnd from running in failed transactions. That's fine and dandy,
		 * but unfortunately GPDB relies on ExecutorEnd to for some cleanup
		 * work, like terminating the Gang. So we in GPDB, we must run ExecutorEnd.
		 * We really should refactor the resource management in dispatcher and
		 * gangs, e.g. to use ResourceOwners instead. But until that's done,
		 * we cannot skip ExecutorEnd.
		 */
#if 0
		/*
		 * If it was created in the current transaction, we can't do normal
		 * shutdown on a READY portal either; it might refer to objects
		 * created in the failed transaction.  See comments in
		 * AtSubAbort_Portals.
		 */
		if (portal->status == PORTAL_READY)
			MarkPortalFailed(portal);
#endif

		/*
		 * Allow portalcmds.c to clean up the state it knows about, if we
		 * haven't already.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			(*portal->cleanup) (portal);
			portal->cleanup = NULL;
		}

		/* drop cached plan reference, if any */
		PortalReleaseCachedPlan(portal);

		/*
		 * Any resources belonging to the portal will be released in the
		 * upcoming transaction-wide cleanup; they will be gone before we run
		 * PortalDrop.
		 */
		portal->resowner = NULL;

		/*
		 * Although we can't delete the portal data structure proper, we can
		 * release any memory in subsidiary contexts, such as executor state.
		 * The cleanup hook was the last thing that might have needed data
		 * there.
		 */
		MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
	}
}

/*
 * Post-abort cleanup for portals.
 *
 * Delete all portals not held over from prior transactions.  */
void
AtCleanup_Portals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Do nothing to cursors held over from a previous transaction */
		if (portal->createSubid == InvalidSubTransactionId)
		{
			Assert(portal->status != PORTAL_ACTIVE);
			Assert(portal->resowner == NULL);
			continue;
		}

		/*
		 * If a portal is still pinned, forcibly unpin it. PortalDrop will not
		 * let us drop the portal otherwise. Whoever pinned the portal was
		 * interrupted by the abort too and won't try to use it anymore.
		 */
		if (portal->portalPinned)
			portal->portalPinned = false;

		/*
		 * We had better not call any user-defined code during cleanup, so if
		 * the cleanup hook hasn't been run yet, too bad; we'll just skip it.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* Zap it. */
		PortalDrop(portal, false);
	}
}

/*
 * Pre-subcommit processing for portals.
 *
 * Reassign portals created or used in the current subtransaction to the
 * parent subtransaction.
 */
void
AtSubCommit_Portals(SubTransactionId mySubid,
					SubTransactionId parentSubid,
					ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid == mySubid)
		{
			portal->createSubid = parentSubid;
			if (portal->resowner)
				ResourceOwnerNewParent(portal->resowner, parentXactOwner);
		}
		if (portal->activeSubid == mySubid)
			portal->activeSubid = parentSubid;
	}
}

/*
 * Subtransaction abort handling for portals.
 *
 * Deactivate portals created or used during the failed subtransaction.
 * Note that per AtSubCommit_Portals, this will catch portals created/used
 * in descendants of the subtransaction too.
 *
 * We don't destroy any portals here; that's done in AtSubCleanup_Portals.
 */
void
AtSubAbort_Portals(SubTransactionId mySubid,
				   SubTransactionId parentSubid,
				   ResourceOwner myXactOwner,
				   ResourceOwner parentXactOwner)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		/* Was it created in this subtransaction? */
		if (portal->createSubid != mySubid)
		{
			/* No, but maybe it was used in this subtransaction? */
			if (portal->activeSubid == mySubid)
			{
				/* Maintain activeSubid until the portal is removed */
				portal->activeSubid = parentSubid;

				/*
				 * Upper-level portals that failed while running in this
				 * subtransaction must be forced into FAILED state, for the
				 * same reasons discussed below.
				 *
				 * We assume we can get away without forcing upper-level READY
				 * portals to fail, even if they were run and then suspended.
				 * In theory a suspended upper-level portal could have
				 * acquired some references to objects that are about to be
				 * destroyed, but there should be sufficient defenses against
				 * such cases: the portal's original query cannot contain such
				 * references, and any references within, say, cached plans of
				 * PL/pgSQL functions are not from active queries and should
				 * be protected by revalidation logic.
				 */
				if (portal->status == PORTAL_ACTIVE)
					MarkPortalFailed(portal);

				/*
				 * Also, if we failed it during the current subtransaction
				 * (either just above, or earlier), reattach its resource
				 * owner to the current subtransaction's resource owner, so
				 * that any resources it still holds will be released while
				 * cleaning up this subtransaction.  This prevents some corner
				 * cases wherein we might get Asserts or worse while cleaning
				 * up objects created during the current subtransaction
				 * (because they're still referenced within this portal).
				 */
				if (portal->status == PORTAL_FAILED && portal->resowner)
				{
					ResourceOwnerNewParent(portal->resowner, myXactOwner);
					portal->resowner = NULL;
				}
			}
			/* Done if it wasn't created in this subtransaction */
			continue;
		}

		/*
		 * Force any live portals of my own subtransaction into FAILED state.
		 * We have to do this because they might refer to objects created or
		 * changed in the failed subtransaction, leading to crashes within
		 * ExecutorEnd when portalcmds.c tries to close down the portal.
		 */
		// GPDB_90_MERGE_FIXME: Not in READY portals. See comment in AtAbort_Portals.
		if (//portal->status == PORTAL_READY ||
			portal->status == PORTAL_ACTIVE)
			MarkPortalFailed(portal);

		/*
		 * Allow portalcmds.c to clean up the state it knows about, if we
		 * haven't already.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			(*portal->cleanup) (portal);
			portal->cleanup = NULL;
		}

		/* drop cached plan reference, if any */
		PortalReleaseCachedPlan(portal);

		/*
		 * Any resources belonging to the portal will be released in the
		 * upcoming transaction-wide cleanup; they will be gone before we run
		 * PortalDrop.
		 */
		portal->resowner = NULL;

		/*
		 * Although we can't delete the portal data structure proper, we can
		 * release any memory in subsidiary contexts, such as executor state.
		 * The cleanup hook was the last thing that might have needed data
		 * there.
		 */
		MemoryContextDeleteChildren(PortalGetHeapMemory(portal));
	}
}

/*
 * Post-subabort cleanup for portals.
 *
 * Drop all portals created in the failed subtransaction (but note that
 * we will not drop any that were reassigned to the parent above).
 */
void
AtSubCleanup_Portals(SubTransactionId mySubid)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->createSubid != mySubid)
			continue;

		/*
		 * If a portal is still pinned, forcibly unpin it. PortalDrop will not
		 * let us drop the portal otherwise. Whoever pinned the portal was
		 * interrupted by the abort too and won't try to use it anymore.
		 */
		if (portal->portalPinned)
			portal->portalPinned = false;

		/*
		 * We had better not call any user-defined code during cleanup, so if
		 * the cleanup hook hasn't been run yet, too bad; we'll just skip it.
		 */
		if (PointerIsValid(portal->cleanup))
		{
			elog(WARNING, "skipping cleanup for portal \"%s\"", portal->name);
			portal->cleanup = NULL;
		}

		/* Zap it. */
		PortalDrop(portal, false);
	}
}

/*
 * At exit ensure all resource locks get released (holdable cursors).
 */
void
AtExitCleanup_ResPortals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	if (PortalHashTable == NULL)
		return;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (IsResQueueLockedForPortal(portal))
			ResUnLockPortal(portal);

	}
}


/*
 * TotalResPortalIncrements --  Calculate increment totals and count of portals
 * for all my portals with a given queueid.
 *
 * Note:
 *	Requires the ResQueueLock to be held before calling.
 *	We are deliberately obscure about the type of totalIncrements as
 *	don't want portal.h to need to include resscheuler.h.
 */
void
TotalResPortalIncrements(int pid, Oid queueid, Cost *totalIncrements, int *num)
{
	HASH_SEQ_STATUS 	status;
	PortalHashEnt		*hentry;
	ResPortalIncrement	*incrementSet;
	ResPortalTag		portalTag;

	int					i;

	/* ensure the total is initialized to zero */
	for (i = 0; i < NUM_RES_LIMIT_TYPES; i++)
		totalIncrements[i] = 0;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;


		if (portal->queueId == queueid)
		{
			/*
			 * Get the increment for this portal, skip if we can't find an
			 * increment, as that portal is uninteresting.
			 */
			MemSet(&portalTag, 0, sizeof(ResPortalTag));
			portalTag.pid = pid;
			portalTag.portalId = portal->portalId;

			incrementSet = ResIncrementFind(&portalTag);
			if (!incrementSet)
				continue;

			/* Count it. */
			(*num)++;

			/* Add its increments to the total. */
			for (i = 0; i < NUM_RES_LIMIT_TYPES; i++)
				totalIncrements[i] += incrementSet->increments[i];
		}
	}
}

/* Find all available cursors */
Datum
pg_cursor(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	TupleDesc	tupdesc;
	Tuplestorestate *tupstore;
	MemoryContext per_query_ctx;
	MemoryContext oldcontext;
	HASH_SEQ_STATUS hash_seq;
	PortalHashEnt *hentry;

	/* check to see if caller supports us returning a tuplestore */
	if (rsinfo == NULL || !IsA(rsinfo, ReturnSetInfo))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("set-valued function called in context that cannot accept a set")));
	if (!(rsinfo->allowedModes & SFRM_Materialize))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("materialize mode required, but it is not " \
						"allowed in this context")));

	/* need to build tuplestore in query context */
	per_query_ctx = rsinfo->econtext->ecxt_per_query_memory;
	oldcontext = MemoryContextSwitchTo(per_query_ctx);

	/*
	 * build tupdesc for result tuples. This must match the definition of the
	 * pg_cursors view in system_views.sql
	 */
	tupdesc = CreateTemplateTupleDesc(6, false);
	TupleDescInitEntry(tupdesc, (AttrNumber) 1, "name",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 2, "statement",
					   TEXTOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 3, "is_holdable",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 4, "is_binary",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 5, "is_scrollable",
					   BOOLOID, -1, 0);
	TupleDescInitEntry(tupdesc, (AttrNumber) 6, "creation_time",
					   TIMESTAMPTZOID, -1, 0);

	/*
	 * We put all the tuples into a tuplestore in one scan of the hashtable.
	 * This avoids any issue of the hashtable possibly changing between calls.
	 */
	tupstore =
		tuplestore_begin_heap(rsinfo->allowedModes & SFRM_Materialize_Random,
							  false, work_mem);

	/* generate junk in short-term context */
	MemoryContextSwitchTo(oldcontext);

	hash_seq_init(&hash_seq, PortalHashTable);
	while ((hentry = hash_seq_search(&hash_seq)) != NULL)
	{
		Portal		portal = hentry->portal;
		Datum		values[6];
		bool		nulls[6];

		/* report only "visible" entries */
		if (!portal->visible)
			continue;

		MemSet(nulls, 0, sizeof(nulls));

		values[0] = CStringGetTextDatum(portal->name);
		values[1] = CStringGetTextDatum(portal->sourceText);
		values[2] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_HOLD);
		values[3] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_BINARY);
		values[4] = BoolGetDatum(portal->cursorOptions & CURSOR_OPT_SCROLL);
		values[5] = TimestampTzGetDatum(portal->creation_time);

		tuplestore_putvalues(tupstore, tupdesc, values, nulls);
	}

	/* clean up and return the tuplestore */
	tuplestore_donestoring(tupstore);

	rsinfo->returnMode = SFRM_Materialize;
	rsinfo->setResult = tupstore;
	rsinfo->setDesc = tupdesc;

	return (Datum) 0;
}

bool
ThereAreNoReadyPortals(void)
{
	HASH_SEQ_STATUS status;
	PortalHashEnt *hentry;

	hash_seq_init(&status, PortalHashTable);

	while ((hentry = (PortalHashEnt *) hash_seq_search(&status)) != NULL)
	{
		Portal		portal = hentry->portal;

		if (portal->status == PORTAL_READY)
			return false;
	}

	return true;
}

/* Find all Parallel Retrieve cursors and return a list of Portals */
List *
GetAllParallelRetrieveCursorPortals(void)
{
	List		*portals;
	PortalHashEnt	*hentry;
	HASH_SEQ_STATUS	status;

	if (PortalHashTable == NULL)
		return NULL;

	portals = NULL;
	hash_seq_init(&status, PortalHashTable);
	while ((hentry = hash_seq_search(&status)) != NULL)
	{
		if (PortalIsParallelRetrieveCursor(hentry->portal) &&
			hentry->portal->queryDesc != NULL)
			portals = lappend(portals, hentry->portal);
	}

	return portals;
}

/* Return the amount of parallel retrieve cursors */
int
GetNumOfParallelRetrieveCursors(void)
{
	List   *portals;
	int		sum;

	portals = GetAllParallelRetrieveCursorPortals();
	sum = list_length(portals);

	list_free(portals);
	return sum;
}
