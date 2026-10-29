/* Shared sequence allocation. Canonical sequence definitions never store this locator. */
#ifndef PG_DOLT_SEQUENCE_H
#define PG_DOLT_SEQUENCE_H
#include "catalog/genbki.h"
#include "catalog/pg_dolt_sequence_d.h"
CATALOG(pg_dolt_sequence,9146,DoltSequenceRelationId)
{
	NameData	seqnamespace;
	NameData	seqname;
	Oid			seqstorage;
	int64		seqincrement;
} FormData_pg_dolt_sequence;
typedef FormData_pg_dolt_sequence * Form_pg_dolt_sequence;

DECLARE_UNIQUE_INDEX_PKEY(pg_dolt_sequence_key_index, 9147, DoltSequenceKeyIndexId, pg_dolt_sequence, btree(seqnamespace name_ops, seqname name_ops));
#endif
