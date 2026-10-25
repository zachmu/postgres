/*-------------------------------------------------------------------------
 *
 * pg_dolt_lock.h
 *	  Lock records for rows and unique keys of prolly relations.
 *
 * A row of a prolly relation has no tuple header of its own, so its row locks
 * live on a record here: one per (relation storage, branch, handle), created
 * the first time the row is locked, updated or deleted on that branch.
 * Locking takes PostgreSQL's ordinary heap tuple lock on the record, which
 * supplies the lock strengths, multixacts, waits, deadlock detection, NOWAIT
 * and SKIP LOCKED. Records for different branches differ, so a lock on one
 * branch never blocks another. Records never hold row data and are never
 * copied when a branch is created.
 *
 * Updating a row gives its new version a new handle, recorded as the heir of
 * the old version's record, so a transaction that waited on the old version
 * can find the new one.
 *
 * Unique indexes reserve keys with records here too, keyed by the index
 * storage and a hash of the key, so concurrent inserts of equal keys wait.
 *
 * src/include/catalog/pg_dolt_lock.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_LOCK_H
#define PG_DOLT_LOCK_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_lock_d.h"

CATALOG(pg_dolt_lock,9129,DoltLockRelationId)
{
	Oid			lockspace BKI_FORCE_NOT_NULL;	/* tablespace of the storage */
	Oid			lockfile BKI_FORCE_NOT_NULL;	/* file number of the storage */
	int64		lockhandle BKI_FORCE_NOT_NULL;	/* row handle or key hash */
	int64		lockheir BKI_FORCE_NOT_NULL;	/* successor handle, or 0 */
#ifdef CATALOG_VARLEN
	text		lockbranch BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_lock;

typedef FormData_pg_dolt_lock *Form_pg_dolt_lock;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_lock_key_index, 9130, DoltLockKeyIndexId, pg_dolt_lock, btree(lockspace oid_ops, lockfile oid_ops, lockhandle int8_ops, lockbranch text_ops));

#endif							/* PG_DOLT_LOCK_H */
