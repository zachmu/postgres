/*-------------------------------------------------------------------------
 *
 * pg_dolt_identity.h
 *	  Durable OIDs of prolly objects on named branch heads.
 *
 * Each table and index on a branch, and each table's row and array types,
 * has one OID shared by every backend and by hot standbys, which replicate
 * this catalog. Relation locks, invalidation messages and statistics use it.
 * Rows are created when a branch or object is created and deleted when either
 * is dropped; renaming a branch deletes and re-creates them. Revisions
 * (commits, tags, AS OF) never get rows here: they are read-only and use
 * backend-local handles.
 *
 * Durable OIDs come from a range the ordinary OID counter never assigns, so a
 * lookup can tell a durable OID from a physical one without a catalog probe.
 * They never enter prolly storage or any hash.
 *
 * src/include/catalog/pg_dolt_identity.h
 *
 *-------------------------------------------------------------------------
 */
#ifndef PG_DOLT_IDENTITY_H
#define PG_DOLT_IDENTITY_H

#include "catalog/genbki.h"
#include "catalog/pg_dolt_identity_d.h"

CATALOG(pg_dolt_identity,9132,DoltIdentityRelationId)
{
	Oid			identoid BKI_FORCE_NOT_NULL;	/* the durable OID */
	char		identkind BKI_FORCE_NOT_NULL;	/* see below */
#ifdef CATALOG_VARLEN
	text		identbranch BKI_FORCE_NOT_NULL;
	text		identname BKI_FORCE_NOT_NULL;	/* qualified relation name */
#endif
} FormData_pg_dolt_identity;

typedef FormData_pg_dolt_identity *Form_pg_dolt_identity;

#ifdef EXPOSE_TO_CLIENT_CODE
#define DOLT_IDENTITY_RELATION	'r'		/* table or index */
#define DOLT_IDENTITY_ROWTYPE	't'		/* a table's row type */
#define DOLT_IDENTITY_ARRAYTYPE	'a'		/* that row type's array type */
#endif

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_identity_oid_index, 9133, DoltIdentityOidIndexId, pg_dolt_identity, btree(identoid oid_ops));
DECLARE_UNIQUE_INDEX(pg_dolt_identity_name_index, 9134, DoltIdentityNameIndexId, pg_dolt_identity, btree(identbranch text_ops, identkind char_ops, identname text_ops));

#endif							/* PG_DOLT_IDENTITY_H */
