/*-------------------------------------------------------------------------
 *
 * matview.c
 *	  materialized view support
 *
 * Portions Copyright (c) 1996-2012, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/matview.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/catalog.h"
#include "catalog/heap.h"
#include "catalog/namespace.h"
#include "commands/cluster.h"
#include "commands/matview.h"
#include "commands/tablecmds.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "rewrite/rewriteHandler.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "tcop/tcopprot.h"
#include "utils/snapmgr.h"


typedef struct
{
	DestReceiver pub;			/* publicly-known function pointers */
	Oid			transientoid;	/* OID of new heap into which to store */
	/* These fields are filled by transientrel_startup: */
	Relation	transientrel;	/* relation to write to */
	CommandId	output_cid;		/* cmin to insert in output tuples */
	int			hi_options;		/* heap_insert performance options */
	BulkInsertState bistate;	/* bulk insert state */
} DR_transientrel;

static void transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo);
static void transientrel_receive(TupleTableSlot *slot, DestReceiver *self);
static void transientrel_shutdown(DestReceiver *self);
static void transientrel_destroy(DestReceiver *self);
static void refresh_matview_datafill(DestReceiver *dest, Query *query,
									 const char *queryString);

/*
 * SetMatViewToPopulated
 *		Indicate that the materialized view has been populated by its query.
 *
 * NOTE: The heap starts out in a state that doesn't look scannable, and can
 * only transition from there to scannable at the time a new heap is created.
 *
 * NOTE: caller must be holding an appropriate lock on the relation.
 */
void
SetMatViewToPopulated(Relation relation)
{
	Page        page;

	Assert(relation->rd_rel->relkind == RELKIND_MATVIEW);
	Assert(relation->rd_ispopulated == false);

	page = (Page) palloc(BLCKSZ);
	PageInit(page, BLCKSZ, 0);

	if (RelationNeedsWAL(relation))
		log_newpage(&(relation->rd_node), MAIN_FORKNUM, 0, page);

	RelationOpenSmgr(relation);

	PageSetChecksumInplace(page, 0);
	smgrextend(relation->rd_smgr, MAIN_FORKNUM, 0, (char *) page, true);

	pfree(page);

	smgrimmedsync(relation->rd_smgr, MAIN_FORKNUM);

	RelationCacheInvalidateEntry(relation->rd_id);
}

/*
 * ExecRefreshMatView -- execute a REFRESH MATERIALIZED VIEW command
 *
 * This refreshes the materialized view by creating a new table and swapping
 * the relfilenodes of the new table and the old materialized view, so the OID
 * of the original materialized view is preserved. Thus we do not lose GRANT
 * nor references to this materialized view.
 *
 * If WITH NO DATA was specified, this is effectively like a TRUNCATE;
 * otherwise it is like a TRUNCATE followed by an INSERT using the SELECT
 * statement associated with the materialized view.  The statement node's
 * skipData field is used to indicate that the clause was used.
 *
 * Indexes are rebuilt too, via REINDEX. Since we are effectively bulk-loading
 * the new heap, it's better to create the indexes afterwards than to fill them
 * incrementally while we load.
 *
 * The scannable state is changed based on whether the contents reflect the
 * result set of the materialized view's query.
 */
void
ExecRefreshMatView(RefreshMatViewStmt *stmt, const char *queryString,
				  ParamListInfo params, char *completionTag)
{
	Oid			matviewOid;
	Relation	matviewRel;
	RewriteRule *rule;
	List	   *actions;
	Query	   *dataQuery;
	Oid			tableSpace;
	Oid			OIDNewHeap;
	DestReceiver *dest;

	/*
	 * Get a lock until end of transaction.
	 */
	matviewOid = RangeVarGetRelidExtended(stmt->relation,
										   AccessExclusiveLock, false, false,
										   RangeVarCallbackOwnsTable, NULL);
	matviewRel = heap_open(matviewOid, NoLock);

	/* Make sure it is a materialized view. */
	if (matviewRel->rd_rel->relkind != RELKIND_MATVIEW)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("\"%s\" is not a materialized view",
						RelationGetRelationName(matviewRel))));

	/*
	 * We're not using materialized views in the system catalogs.
	 */
	Assert(!IsSystemRelation(matviewRel));

	Assert(!matviewRel->rd_rel->relhasoids);

	/*
	 * Check that everything is correct for a refresh. Problems at this point
	 * are internal errors, so elog is sufficient.
	 */
	if (matviewRel->rd_rel->relhasrules == false ||
		matviewRel->rd_rules->numLocks < 1)
		elog(ERROR,
			 "materialized view \"%s\" is missing rewrite information",
			 RelationGetRelationName(matviewRel));

	if (matviewRel->rd_rules->numLocks > 1)
		elog(ERROR,
			 "materialized view \"%s\" has too many rules",
			 RelationGetRelationName(matviewRel));

	rule = matviewRel->rd_rules->rules[0];
	if (rule->event != CMD_SELECT || !(rule->isInstead))
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a SELECT INSTEAD OF rule",
			 RelationGetRelationName(matviewRel));

	actions = rule->actions;
	if (list_length(actions) != 1)
		elog(ERROR,
			 "the rule for materialized view \"%s\" is not a single action",
			 RelationGetRelationName(matviewRel));

	/*
	 * The stored query was rewritten at the time of the MV definition, but
	 * has not been scribbled on by the planner.
	 */
	dataQuery = (Query *) linitial(actions);
	Assert(IsA(dataQuery, Query));

	/*
	 * Check for active uses of the relation in the current transaction, such
	 * as open scans.
	 *
	 * NB: We count on this to protect us against problems with refreshing the
	 * data using HEAP_INSERT_FROZEN.
	 */
	CheckTableNotInUse(matviewRel, "REFRESH MATERIALIZED VIEW");

	tableSpace = matviewRel->rd_rel->reltablespace;

	heap_close(matviewRel, NoLock);

	/* Create the transient table that will receive the regenerated data. */
	OIDNewHeap = make_new_heap(matviewOid, tableSpace);
	dest = CreateTransientRelDestReceiver(OIDNewHeap);

	if (!stmt->skipData)
		refresh_matview_datafill(dest, dataQuery, queryString);

	/*
	 * Swap the physical files of the target and transient tables, then
	 * rebuild the target's indexes and throw away the transient table.
	 */
	finish_heap_swap(matviewOid, OIDNewHeap, false, false, true, true,
					 RecentXmin, ReadNextMultiXactId());

	RelationCacheInvalidateEntry(matviewOid);
}

/*
 * refresh_matview_datafill
 */
static void
refresh_matview_datafill(DestReceiver *dest, Query *query,
						 const char *queryString)
{
	List       *rewritten;
	PlannedStmt *plan;
	QueryDesc  *queryDesc;
	List	   *rtable;
	RangeTblEntry	*initial_rte;
	RangeTblEntry	*second_rte;

	rewritten = QueryRewrite((Query *) copyObject(query));

	/* SELECT should never rewrite to more or less than one SELECT query */
	if (list_length(rewritten) != 1)
		elog(ERROR, "unexpected rewrite result for REFRESH MATERIALIZED VIEW");
	query = (Query *) linitial(rewritten);

	/* Check for user-requested abort. */
	CHECK_FOR_INTERRUPTS();

	/*
	 * Kludge here to allow refresh of a materialized view which is invalid
	 * (that is, it was created or refreshed WITH NO DATA. We flag the first
	 * two RangeTblEntry list elements, which were added to the front of the
	 * rewritten Query to keep the rules system happy, with the isResultRel
	 * flag to indicate that it is OK if they are flagged as invalid. See
	 * UpdateRangeTableOfViewParse() for details.
	 *
	 * NOTE: The rewrite has switched the frist two RTEs, but they are still
	 * in the first two positions. If that behavior changes, the asserts here
	 * will fail.
	 */
	rtable = query->rtable;
	initial_rte = ((RangeTblEntry *) linitial(rtable));
	Assert(strcmp(initial_rte->alias->aliasname, "new"));
	initial_rte->isResultRel = true;
	second_rte = ((RangeTblEntry *) lsecond(rtable));
	Assert(strcmp(second_rte->alias->aliasname, "old"));
	second_rte->isResultRel = true;

	/* Plan the query which will generate data for the refresh. */
	plan = pg_plan_query(query, 0, NULL);

	/*
	 * Use a snapshot with an updated command ID to ensure this query sees
	 * results of any previously executed queries.	(This could only matter if
	 * the planner executed an allegedly-stable function that changed the
	 * database contents, but let's do it anyway to be safe.)
	 */
	PushCopiedSnapshot(GetActiveSnapshot());
	UpdateActiveSnapshotCommandId();

	/* Create a QueryDesc, redirecting output to our tuple receiver */
	queryDesc = CreateQueryDesc(plan, queryString,
								GetActiveSnapshot(), InvalidSnapshot,
								dest, NULL, 0);

	/* call ExecutorStart to prepare the plan for execution */
	ExecutorStart(queryDesc, EXEC_FLAG_WITHOUT_OIDS);

	/* run the plan */
	ExecutorRun(queryDesc, ForwardScanDirection, 0L);

	/* and clean up */
	ExecutorFinish(queryDesc);
	ExecutorEnd(queryDesc);

	FreeQueryDesc(queryDesc);

	PopActiveSnapshot();
}

DestReceiver *
CreateTransientRelDestReceiver(Oid transientoid)
{
	DR_transientrel *self = (DR_transientrel *) palloc0(sizeof(DR_transientrel));

	self->pub.receiveSlot = transientrel_receive;
	self->pub.rStartup = transientrel_startup;
	self->pub.rShutdown = transientrel_shutdown;
	self->pub.rDestroy = transientrel_destroy;
	self->pub.mydest = DestTransientRel;
	self->transientoid = transientoid;

	return (DestReceiver *) self;
}

/*
 * transientrel_startup --- executor startup
 */
static void
transientrel_startup(DestReceiver *self, int operation, TupleDesc typeinfo)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	Relation transientrel;

	transientrel = heap_open(myState->transientoid, NoLock);

	/*
	 * Fill private fields of myState for use by later routines
	 */
	myState->transientrel = transientrel;
	myState->output_cid = GetCurrentCommandId(true);

	/*
	 * We can skip WAL-logging the insertions, unless PITR or streaming
	 * replication is in use. We can skip the FSM in any case.
	 */
	myState->hi_options = HEAP_INSERT_SKIP_FSM | HEAP_INSERT_FROZEN;
	if (!XLogIsNeeded())
		myState->hi_options |= HEAP_INSERT_SKIP_WAL;
	myState->bistate = GetBulkInsertState();

	SetMatViewToPopulated(transientrel);

	/* Not using WAL requires smgr_targblock be initially invalid */
	Assert(RelationGetTargetBlock(transientrel) == InvalidBlockNumber);
}

/*
 * transientrel_receive --- receive one tuple
 */
static void
transientrel_receive(TupleTableSlot *slot, DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;
	HeapTuple	tuple;

	/*
	 * get the heap tuple out of the tuple table slot, making sure we have a
	 * writable copy
	 */
	tuple = ExecMaterializeSlot(slot);

	heap_insert(myState->transientrel,
				tuple,
				myState->output_cid,
				myState->hi_options,
				myState->bistate);

	/* We know this is a newly created relation, so there are no indexes */
}

/*
 * transientrel_shutdown --- executor end
 */
static void
transientrel_shutdown(DestReceiver *self)
{
	DR_transientrel *myState = (DR_transientrel *) self;

	FreeBulkInsertState(myState->bistate);

	/* If we skipped using WAL, must heap_sync before commit */
	if (myState->hi_options & HEAP_INSERT_SKIP_WAL)
		heap_sync(myState->transientrel);

	/* close transientrel, but keep lock until commit */
	heap_close(myState->transientrel, NoLock);
	myState->transientrel = NULL;
}

/*
 * transientrel_destroy --- release DestReceiver object
 */
static void
transientrel_destroy(DestReceiver *self)
{
	pfree(self);
}
