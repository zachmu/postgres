/*-------------------------------------------------------------------------
 *
 * pg_dolt_lock.h
 *	  Lock records for rows and unique keys of prolly relations.
 *
 * A prolly row has no tuple header of its own, so each row version is shadowed
 * by a lock record: a heap tuple that changes as the row does. Locking a row
 * locks its record with heap_lock_tuple; updating the row updates the record
 * so that it names the new version's handle; deleting the row deletes the
 * record. Heap then supplies the lock strengths, multixacts, waits, deadlock
 * detection, NOWAIT, SKIP LOCKED, update chains and lock propagation.
 *
 * Records are created lazily, frozen, the first time a row is locked, updated
 * or deleted on its branch. They are keyed by branch, so a lock on one branch
 * never blocks another, and they are never copied when a branch is created.
 * The primary key names the record's first handle, which never changes, so
 * updating a record is never a key update. A second index finds a record by
 * its current handle.
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
	int64		lockroot BKI_FORCE_NOT_NULL;	/* first handle; never changes */
	int64		lockhandle BKI_FORCE_NOT_NULL;	/* current handle, or key hash */
#ifdef CATALOG_VARLEN
	text		lockbranch BKI_FORCE_NOT_NULL;
#endif
} FormData_pg_dolt_lock;

typedef FormData_pg_dolt_lock *Form_pg_dolt_lock;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_lock_key_index, 9130, DoltLockKeyIndexId, pg_dolt_lock, btree(lockspace oid_ops, lockfile oid_ops, lockroot int8_ops, lockbranch text_ops));
DECLARE_INDEX(pg_dolt_lock_handle_index, 9131, DoltLockHandleIndexId, pg_dolt_lock, btree(lockspace oid_ops, lockfile oid_ops, lockhandle int8_ops, lockbranch text_ops));

#endif							/* PG_DOLT_LOCK_H */
