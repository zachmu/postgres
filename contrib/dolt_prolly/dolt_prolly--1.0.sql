CREATE FUNCTION dolt_prolly_table_handler(internal) RETURNS table_am_handler
AS 'MODULE_PATHNAME', 'dolt_prolly_table_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD dolt_prolly_table TYPE TABLE HANDLER dolt_prolly_table_handler;
CREATE FUNCTION dolt_prolly_index_handler(internal) RETURNS index_am_handler
AS 'MODULE_PATHNAME', 'dolt_prolly_index_handler' LANGUAGE C STRICT;
CREATE ACCESS METHOD dolt_prolly_index TYPE INDEX HANDLER dolt_prolly_index_handler;
CREATE FUNCTION dolt_prolly_root(regclass) RETURNS text
AS 'MODULE_PATHNAME', 'dolt_prolly_root' LANGUAGE C STRICT;
CREATE OPERATOR CLASS dolt_int2_ops DEFAULT FOR TYPE int2 USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (int2, int2),
OPERATOR 2 pg_catalog.<= (int2, int2),
OPERATOR 3 pg_catalog.= (int2, int2),
OPERATOR 4 pg_catalog.>= (int2, int2),
OPERATOR 5 pg_catalog.> (int2, int2),
FUNCTION 1 pg_catalog.btint2cmp(int2, int2);
CREATE OPERATOR CLASS dolt_int4_ops DEFAULT FOR TYPE int4 USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (int4, int4),
OPERATOR 2 pg_catalog.<= (int4, int4),
OPERATOR 3 pg_catalog.= (int4, int4),
OPERATOR 4 pg_catalog.>= (int4, int4),
OPERATOR 5 pg_catalog.> (int4, int4),
FUNCTION 1 pg_catalog.btint4cmp(int4, int4);
CREATE OPERATOR CLASS dolt_int8_ops DEFAULT FOR TYPE int8 USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (int8, int8),
OPERATOR 2 pg_catalog.<= (int8, int8),
OPERATOR 3 pg_catalog.= (int8, int8),
OPERATOR 4 pg_catalog.>= (int8, int8),
OPERATOR 5 pg_catalog.> (int8, int8),
FUNCTION 1 pg_catalog.btint8cmp(int8, int8);
CREATE OPERATOR CLASS dolt_bool_ops DEFAULT FOR TYPE bool USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (bool, bool),
OPERATOR 2 pg_catalog.<= (bool, bool),
OPERATOR 3 pg_catalog.= (bool, bool),
OPERATOR 4 pg_catalog.>= (bool, bool),
OPERATOR 5 pg_catalog.> (bool, bool),
FUNCTION 1 pg_catalog.btboolcmp(bool, bool);
CREATE OPERATOR CLASS dolt_text_ops DEFAULT FOR TYPE text USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (text, text),
OPERATOR 2 pg_catalog.<= (text, text),
OPERATOR 3 pg_catalog.= (text, text),
OPERATOR 4 pg_catalog.>= (text, text),
OPERATOR 5 pg_catalog.> (text, text),
FUNCTION 1 pg_catalog.bttextcmp(text, text);
CREATE OPERATOR CLASS dolt_bytea_ops DEFAULT FOR TYPE bytea USING dolt_prolly_index AS
OPERATOR 1 pg_catalog.< (bytea, bytea),
OPERATOR 2 pg_catalog.<= (bytea, bytea),
OPERATOR 3 pg_catalog.= (bytea, bytea),
OPERATOR 4 pg_catalog.>= (bytea, bytea),
OPERATOR 5 pg_catalog.> (bytea, bytea),
FUNCTION 1 pg_catalog.byteacmp(bytea, bytea);

REVOKE ALL ON FUNCTION dolt_prolly_root(regclass) FROM PUBLIC;
