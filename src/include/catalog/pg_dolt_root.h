/*-------------------------------------------------------------------------
 *
 * pg_dolt_root.h
 *	  Committed working roots of prolly relations, per branch.
 *
 * One row per (relation storage, branch) holds the roots of that relation's
 * trees in the chunk store: content, and for tables the handle and
 * key-to-handle trees. Rows change only when a transaction publishes, just
 * before it commits, so ordinary MVCC selects the roots a snapshot sees.
 * Storage is identified by tablespace and file number, so TRUNCATE (a new
 * file) starts from empty roots and rolling it back restores the old ones.
 * Branch names are authoritative; a rename rewrites them.
 *
 * src/include/catalog/pg_dolt_root.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_ROOT_H
#define PG_DOLT_ROOT_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_root_d.h"

CATALOG(pg_dolt_root,9127,DoltRootRelationId)
{
	Oid			rootspace BKI_FORCE_NOT_NULL;	/* tablespace of the storage */
	Oid			rootfile BKI_FORCE_NOT_NULL;	/* file number of the storage */
#ifdef CATALOG_VARLEN
	text		rootbranch BKI_FORCE_NOT_NULL;
	bytea		rootcontent BKI_FORCE_NOT_NULL;
	bytea		roothandles BKI_FORCE_NULL; /* tables only */
	bytea		rootkeys BKI_FORCE_NULL;	/* tables only */
#endif
} FormData_pg_dolt_root;

typedef FormData_pg_dolt_root *Form_pg_dolt_root;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_root_key_index, 9128, DoltRootKeyIndexId, pg_dolt_root, btree(rootspace oid_ops, rootfile oid_ops, rootbranch text_ops));

#endif							/* PG_DOLT_ROOT_H */
