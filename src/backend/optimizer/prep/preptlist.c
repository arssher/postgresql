/*-------------------------------------------------------------------------
 *
 * preptlist.c
 *	  Routines to preprocess the parse tree target list
 *
 * For INSERT and UPDATE queries, the targetlist must contain an entry for
 * each attribute of the target relation in the correct order.  For UPDATE and
 * DELETE queries, it must also contain a junk tlist entry needed to allow the
 * executor to identify the physical locations of the rows to be updated or
 * deleted.  For all query types, we may need to add junk tlist entries for
 * Vars used in the RETURNING list and row ID information needed for SELECT
 * FOR UPDATE locking and/or EvalPlanQual checking.
 *
 * The rewriter's rewriteTargetListIU routine also does preprocessing of the
 * targetlist.  The division of labor between here and there is partially
 * historical, but it's not entirely arbitrary.  In particular, consider an
 * UPDATE across an inheritance tree.  What the rewriter does need be done
 * only once (because it depends only on the properties of the parent
 * relation).  What's done here has to be done over again for each child
 * relation, because it depends on the column list of the child, which might
 * have more columns and/or a different column order than the parent.
 *
 * The fact that rewriteTargetListIU sorts non-resjunk tlist entries by column
 * position, which expand_targetlist depends on, violates the above comment
 * because the sorting is only valid for the parent relation.  In inherited
 * UPDATE cases, adjust_inherited_tlist runs in between to take care of fixing
 * the tlists for child tables to keep expand_targetlist happy.  We do it like
 * that because it's faster in typical non-inherited cases.
 *
 *
 * Portions Copyright (c) 1996-2017, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/optimizer/prep/preptlist.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/heapam.h"
#include "access/sysattr.h"
#include "catalog/pg_type.h"
#include "foreign/fdwapi.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/prep.h"
#include "optimizer/tlist.h"
#include "optimizer/var.h"
#include "parser/parsetree.h"
#include "parser/parse_coerce.h"
#include "utils/rel.h"


static List *expand_targetlist(List *tlist, int command_type,
				  Index result_relation, List *range_table);
static void rewrite_targetlist(Query *parse,
				   Index result_relation, List *range_table);


/*
 * preprocess_targetlist
 *	  Driver for preprocessing the parse tree targetlist.
 *
 *	  Returns the new targetlist.
 */
List *
preprocess_targetlist(PlannerInfo *root)
{
	Query	   *parse = root->parse;
	int			result_relation = parse->resultRelation;
	List	   *range_table = parse->rtable;
	CmdType		command_type = parse->commandType;
	List	   *tlist;
	ListCell   *lc;

	/*
	 * Sanity check: if there is a result relation, it'd better be a real
	 * relation not a subquery.  Else parser or rewriter messed up.
	 */
	if (result_relation)
	{
		RangeTblEntry *rte = rt_fetch(result_relation, range_table);

		if (rte->subquery != NULL || rte->relid == InvalidOid)
			elog(ERROR, "subquery cannot be result relation");
	}

	/*
	 * For UPDATE/DELETE, add a necessary junk column needed to allow the
	 * executor to identify the physical locations of the rows to be updated
	 * or deleted.
	 */
	if (command_type == CMD_UPDATE || command_type == CMD_DELETE)
		rewrite_targetlist(parse, result_relation, range_table);

	tlist = parse->targetList;

	/*
	 * for heap_form_tuple to work, the targetlist must match the exact order
	 * of the attributes. We also need to fill in any missing attributes. -ay
	 * 10/94
	 */
	if (command_type == CMD_INSERT || command_type == CMD_UPDATE)
		tlist = expand_targetlist(tlist, command_type,
								  result_relation, range_table);

	/*
	 * Add necessary junk columns for rowmarked rels.  These values are needed
	 * for locking of rels selected FOR UPDATE/SHARE, and to do EvalPlanQual
	 * rechecking.  See comments for PlanRowMark in plannodes.h.
	 */
	foreach(lc, root->rowMarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(lc);
		Var		   *var;
		char		resname[32];
		TargetEntry *tle;

		/* child rels use the same junk attrs as their parents */
		if (rc->rti != rc->prti)
			continue;

		if (rc->allMarkTypes & ~(1 << ROW_MARK_COPY))
		{
			/* Need to fetch TID */
			var = makeVar(rc->rti,
						  SelfItemPointerAttributeNumber,
						  TIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "ctid%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}
		if (rc->allMarkTypes & (1 << ROW_MARK_COPY))
		{
			/* Need the whole row as a junk var */
			var = makeWholeRowVar(rt_fetch(rc->rti, range_table),
								  rc->rti,
								  0,
								  false);
			snprintf(resname, sizeof(resname), "wholerow%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}

		/* If parent of inheritance tree, always fetch the tableoid too. */
		if (rc->isParent)
		{
			var = makeVar(rc->rti,
						  TableOidAttributeNumber,
						  OIDOID,
						  -1,
						  InvalidOid,
						  0);
			snprintf(resname, sizeof(resname), "tableoid%u", rc->rowmarkId);
			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  pstrdup(resname),
								  true);
			tlist = lappend(tlist, tle);
		}
	}

	/*
	 * If the query has a RETURNING list, add resjunk entries for any Vars
	 * used in RETURNING that belong to other relations.  We need to do this
	 * to make these Vars available for the RETURNING calculation.  Vars that
	 * belong to the result rel don't need to be added, because they will be
	 * made to refer to the actual heap tuple.
	 */
	if (parse->returningList && list_length(parse->rtable) > 1)
	{
		List	   *vars;
		ListCell   *l;

		vars = pull_var_clause((Node *) parse->returningList,
							   PVC_RECURSE_AGGREGATES |
							   PVC_RECURSE_WINDOWFUNCS |
							   PVC_INCLUDE_PLACEHOLDERS);
		foreach(l, vars)
		{
			Var		   *var = (Var *) lfirst(l);
			TargetEntry *tle;

			if (IsA(var, Var) &&
				var->varno == result_relation)
				continue;		/* don't need it */

			if (tlist_member((Expr *) var, tlist))
				continue;		/* already got it */

			tle = makeTargetEntry((Expr *) var,
								  list_length(tlist) + 1,
								  NULL,
								  true);

			tlist = lappend(tlist, tle);
		}
		list_free(vars);
	}

	return tlist;
}

/*
 * preprocess_onconflict_targetlist
 *	  Process ON CONFLICT SET targetlist.
 *
 *	  Returns the new targetlist.
 */
List *
preprocess_onconflict_targetlist(List *tlist, int result_relation, List *range_table)
{
	return expand_targetlist(tlist, CMD_UPDATE, result_relation, range_table);
}


/*****************************************************************************
 *
 *		TARGETLIST EXPANSION
 *
 *****************************************************************************/

/*
 * expand_targetlist
 *	  Given a target list as generated by the parser and a result relation,
 *	  add targetlist entries for any missing attributes, and ensure the
 *	  non-junk attributes appear in proper field order.
 */
static List *
expand_targetlist(List *tlist, int command_type,
				  Index result_relation, List *range_table)
{
	List	   *new_tlist = NIL;
	ListCell   *tlist_item;
	Relation	rel;
	int			attrno,
				numattrs;

	tlist_item = list_head(tlist);

	/*
	 * The rewriter should have already ensured that the TLEs are in correct
	 * order; but we have to insert TLEs for any missing attributes.
	 *
	 * Scan the tuple description in the relation's relcache entry to make
	 * sure we have all the user attributes in the right order.  We assume
	 * that the rewriter already acquired at least AccessShareLock on the
	 * relation, so we need no lock here.
	 */
	rel = heap_open(getrelid(result_relation, range_table), NoLock);

	numattrs = RelationGetNumberOfAttributes(rel);

	for (attrno = 1; attrno <= numattrs; attrno++)
	{
		Form_pg_attribute att_tup = rel->rd_att->attrs[attrno - 1];
		TargetEntry *new_tle = NULL;

		if (tlist_item != NULL)
		{
			TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

			if (!old_tle->resjunk && old_tle->resno == attrno)
			{
				new_tle = old_tle;
				tlist_item = lnext(tlist_item);
			}
		}

		if (new_tle == NULL)
		{
			/*
			 * Didn't find a matching tlist entry, so make one.
			 *
			 * For INSERT, generate a NULL constant.  (We assume the rewriter
			 * would have inserted any available default value.) Also, if the
			 * column isn't dropped, apply any domain constraints that might
			 * exist --- this is to catch domain NOT NULL.
			 *
			 * For UPDATE, generate a Var reference to the existing value of
			 * the attribute, so that it gets copied to the new tuple. But
			 * generate a NULL for dropped columns (we want to drop any old
			 * values).
			 *
			 * When generating a NULL constant for a dropped column, we label
			 * it INT4 (any other guaranteed-to-exist datatype would do as
			 * well). We can't label it with the dropped column's datatype
			 * since that might not exist anymore.  It does not really matter
			 * what we claim the type is, since NULL is NULL --- its
			 * representation is datatype-independent.  This could perhaps
			 * confuse code comparing the finished plan to the target
			 * relation, however.
			 */
			Oid			atttype = att_tup->atttypid;
			int32		atttypmod = att_tup->atttypmod;
			Oid			attcollation = att_tup->attcollation;
			Node	   *new_expr;

			switch (command_type)
			{
				case CMD_INSERT:
					if (!att_tup->attisdropped)
					{
						new_expr = (Node *) makeConst(atttype,
													  -1,
													  attcollation,
													  att_tup->attlen,
													  (Datum) 0,
													  true, /* isnull */
													  att_tup->attbyval);
						new_expr = coerce_to_domain(new_expr,
													InvalidOid, -1,
													atttype,
													COERCE_IMPLICIT_CAST,
													-1,
													false,
													false);
					}
					else
					{
						/* Insert NULL for dropped column */
						new_expr = (Node *) makeConst(INT4OID,
													  -1,
													  InvalidOid,
													  sizeof(int32),
													  (Datum) 0,
													  true, /* isnull */
													  true /* byval */ );
					}
					break;
				case CMD_UPDATE:
					if (!att_tup->attisdropped)
					{
						new_expr = (Node *) makeVar(result_relation,
													attrno,
													atttype,
													atttypmod,
													attcollation,
													0);
					}
					else
					{
						/* Insert NULL for dropped column */
						new_expr = (Node *) makeConst(INT4OID,
													  -1,
													  InvalidOid,
													  sizeof(int32),
													  (Datum) 0,
													  true, /* isnull */
													  true /* byval */ );
					}
					break;
				default:
					elog(ERROR, "unrecognized command_type: %d",
						 (int) command_type);
					new_expr = NULL;	/* keep compiler quiet */
					break;
			}

			new_tle = makeTargetEntry((Expr *) new_expr,
									  attrno,
									  pstrdup(NameStr(att_tup->attname)),
									  false);
		}

		new_tlist = lappend(new_tlist, new_tle);
	}

	/*
	 * The remaining tlist entries should be resjunk; append them all to the
	 * end of the new tlist, making sure they have resnos higher than the last
	 * real attribute.  (Note: although the rewriter already did such
	 * renumbering, we have to do it again here in case we are doing an UPDATE
	 * in a table with dropped columns, or an inheritance child table with
	 * extra columns.)
	 */
	while (tlist_item)
	{
		TargetEntry *old_tle = (TargetEntry *) lfirst(tlist_item);

		if (!old_tle->resjunk)
			elog(ERROR, "targetlist is not sorted correctly");
		/* Get the resno right, but don't copy unnecessarily */
		if (old_tle->resno != attrno)
		{
			old_tle = flatCopyTargetEntry(old_tle);
			old_tle->resno = attrno;
		}
		new_tlist = lappend(new_tlist, old_tle);
		attrno++;
		tlist_item = lnext(tlist_item);
	}

	heap_close(rel, NoLock);

	return new_tlist;
}

/*
 * rewrite_targetlist - rewrite UPDATE/DELETE targetlist as needed
 *
 * This function adds a "junk" TLE that is needed to allow the executor to
 * find the original row for the update or delete.  When the target relation
 * is a regular table, the junk TLE emits the ctid attribute of the original
 * row.  When the target relation is a foreign table, we let the FDW decide
 * what to add.
 */
static void
rewrite_targetlist(Query *parse, Index result_relation, List *range_table)
{
	Var		   *var = NULL;
	const char *attrname;
	TargetEntry *tle;
	RangeTblEntry *target_rte;
	Relation target_relation;

	target_rte = rt_fetch(result_relation, range_table);

	/*
	 * We assume that the relation was already locked by the rewriter, so
	 * we need no lock here.
	 */
	target_relation = heap_open(target_rte->relid, NoLock);

	if (target_relation->rd_rel->relkind == RELKIND_RELATION ||
		target_relation->rd_rel->relkind == RELKIND_MATVIEW ||
		target_relation->rd_rel->relkind == RELKIND_PARTITIONED_TABLE)
	{
		/*
		 * Emit CTID so that executor can find the row to update or delete.
		 */
		var = makeVar(result_relation,
					  SelfItemPointerAttributeNumber,
					  TIDOID,
					  -1,
					  InvalidOid,
					  0);

		attrname = "ctid";
	}
	else if (target_relation->rd_rel->relkind == RELKIND_FOREIGN_TABLE)
	{
		/*
		 * Let the foreign table's FDW add whatever junk TLEs it wants.
		 */
		FdwRoutine *fdwroutine;

		fdwroutine = GetFdwRoutineForRelation(target_relation, false);

		if (fdwroutine->AddForeignUpdateTargets != NULL)
			fdwroutine->AddForeignUpdateTargets(parse, target_rte,
												target_relation);

		/*
		 * If we have a row-level trigger corresponding to the operation, emit
		 * a whole-row Var so that executor will have the "old" row to pass to
		 * the trigger.  Alas, this misses system columns.
		 */
		if (target_relation->trigdesc &&
			((parse->commandType == CMD_UPDATE &&
			  (target_relation->trigdesc->trig_update_after_row ||
			   target_relation->trigdesc->trig_update_before_row)) ||
			 (parse->commandType == CMD_DELETE &&
			  (target_relation->trigdesc->trig_delete_after_row ||
			   target_relation->trigdesc->trig_delete_before_row))))
		{
			var = makeWholeRowVar(target_rte,
								  result_relation,
								  0,
								  false);

			attrname = "wholerow";
		}
	}

	if (var != NULL)
	{
		tle = makeTargetEntry((Expr *) var,
							  list_length(parse->targetList) + 1,
							  pstrdup(attrname),
							  true);

		parse->targetList = lappend(parse->targetList, tle);
	}

	heap_close(target_relation, NoLock);
}


/*
 * Locate PlanRowMark for given RT index, or return NULL if none
 *
 * This probably ought to be elsewhere, but there's no very good place
 */
PlanRowMark *
get_plan_rowmark(List *rowmarks, Index rtindex)
{
	ListCell   *l;

	foreach(l, rowmarks)
	{
		PlanRowMark *rc = (PlanRowMark *) lfirst(l);

		if (rc->rti == rtindex)
			return rc;
	}
	return NULL;
}
