/*-------------------------------------------------------------------------
 *
 * pg_dolt_journal.h
 *	  definition of the "prolly row journal" system catalog (pg_dolt_journal)
 *
 * A branch's small changes to a table are appended here instead of being
 * folded into its prolly trees at once. An entry names the table's storage
 * and branch, the row occurrence's handle, whether the occurrence was added
 * or removed, the row's digest, and for an addition the row's bytes. The
 * trees are rebuilt from a stream of entries in one pass when it grows past
 * a bound or when a canonical root is needed; the entries are then deleted
 * in the same transaction. Nothing canonical is derived from a handle.
 *
 * src/include/catalog/pg_dolt_journal.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_JOURNAL_H
#define PG_DOLT_JOURNAL_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_journal_d.h"

CATALOG(pg_dolt_journal,9135,DoltJournalRelationId)
{
	Oid			jrnspace BKI_FORCE_NOT_NULL;	/* tablespace of the storage */
	Oid			jrnfile BKI_FORCE_NOT_NULL;	/* file number of the storage */
	int64		jrnseq BKI_FORCE_NOT_NULL;	/* order within the stream */
	int64		jrnhandle BKI_FORCE_NOT_NULL;	/* the row occurrence */
	char		jrnkind BKI_FORCE_NOT_NULL;	/* 'i' added, 'd' removed */
#ifdef CATALOG_VARLEN
	text		jrnbranch BKI_FORCE_NOT_NULL;
	bytea		jrndigest BKI_FORCE_NOT_NULL;	/* the row's digest */
	bytea		jrnrow BKI_FORCE_NULL;	/* the row's bytes, when added */
#endif
} FormData_pg_dolt_journal;

typedef FormData_pg_dolt_journal *Form_pg_dolt_journal;

DECLARE_TOAST(pg_dolt_journal, 9137, 9138);

DECLARE_INDEX(pg_dolt_journal_key_index, 9136, DoltJournalKeyIndexId, pg_dolt_journal, btree(jrnspace oid_ops, jrnfile oid_ops, jrnbranch text_ops, jrnseq int8_ops));

#endif							/* PG_DOLT_JOURNAL_H */
