/*-------------------------------------------------------------------------
 *
 * pg_dolt_handle.h
 *	  Allocated row handles of prolly tables, per branch.
 *
 * A row's handle, the TID the executor sees, is derived from the row's
 * digest whenever it can be: the first occurrence of a row in a table takes
 * the leading bits of its digest, so it needs no storage and is the same on
 * every branch and in every revision. Occurrences that cannot take the
 * derived handle, because the row already has one, because another live row
 * shares the digest prefix, or because the handle was recently used, get a
 * handle from the storage's counter instead, and this catalog maps those to
 * their digest. Rows change with the row they map, under ordinary MVCC, so
 * a snapshot sees exactly the allocated handles live under it.
 *
 * src/include/catalog/pg_dolt_handle.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_HANDLE_H
#define PG_DOLT_HANDLE_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_handle_d.h"

CATALOG(pg_dolt_handle,9139,DoltHandleRelationId)
{
	Oid			hdlspace BKI_FORCE_NOT_NULL;	/* tablespace of the storage */
	Oid			hdlfile BKI_FORCE_NOT_NULL;	/* file number of the storage */
	int64		hdlhandle BKI_FORCE_NOT_NULL;	/* an allocated handle */
#ifdef CATALOG_VARLEN
	text		hdlbranch BKI_FORCE_NOT_NULL;
	bytea		hdldigest BKI_FORCE_NOT_NULL;	/* the row occurrence it names */
#endif
} FormData_pg_dolt_handle;

typedef FormData_pg_dolt_handle *Form_pg_dolt_handle;

DECLARE_TOAST(pg_dolt_handle, 9142, 9143);

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_handle_key_index, 9140, DoltHandleKeyIndexId, pg_dolt_handle, btree(hdlspace oid_ops, hdlfile oid_ops, hdlbranch text_ops, hdlhandle int8_ops));
DECLARE_INDEX(pg_dolt_handle_digest_index, 9141, DoltHandleDigestIndexId, pg_dolt_handle, btree(hdlspace oid_ops, hdlfile oid_ops, hdlbranch text_ops, hdldigest bytea_ops));

#endif							/* PG_DOLT_HANDLE_H */
