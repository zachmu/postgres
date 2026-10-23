# Catalog milestone 1: execution and parameter context

SQL and PL/pgSQL now share invocation-context selection for object-ID aliases,
composite values and composite arrays. PL/pgSQL compilation already uses the
shared context-keyed function cache. Its handler now selects the context before
compilation/execution and restores it after errors. Static and EXECUTE SQL inherit
that context through the existing parser/provider interface.

Argument-free PL functions require a separate compilation fix: a `%ROWTYPE`
declaration used to survive checkout with the previous branch's shape. The
compiler now keys declarations by the session's snapshot-visible canonical root
when no enclosing scope exists. It restores the caller scope before execution,
so ordinary procedural DML retains working-branch write authority. Historical
argument-selected invocations remain scoped to their immutable context.

Canonical table rowtypes now expose generated array types, reciprocal typarray /
typelem links, native array I/O/subscript/analyze functions and internal dependency
edges. Names use PostgreSQL's underscore/numeric-suffix/truncation convention,
checking canonical membership rather than physical namespace contents. Runtime
array handles remain outside serialized canonical definitions.

Extended-protocol tests explicitly supply a canonical type OID to Parse, execute
before and after checkout, bind another revision's type, reject an incompatible
record payload, and recover after the Bind error. Explicit type handles continue
to mean their original immutable descriptor; they are not silently reinterpreted
as the new branch's same-named type. Ordinary textual type names resolve in the
applicable current context. SQL PREPARE also exercises composite-array parameters.

The routine-signature audit found OID-vector keys in PROCNAMEARGSNSP, and the
provider still lacks canonical user pg_proc definitions. That integration belongs
to milestone 2 alongside routine admission/capture; this milestone does not
claim versioned CREATE FUNCTION. Procedural test helpers are shared pg_catalog
objects. Trigger/event-trigger routine definitions and the broader dependency
closure remain within milestones 2–4; ordinary native PL regression is retained.

Validation includes the native test_doltgres/test_prolly/core regression/isolation
and PL/pgSQL groups, the growing Doltgres gate, six original wire-binding tests and
18 original PL record scenarios. Two additional exploratory PL scenarios disagree
on native integer error wording and unassigned whole-record behavior; the original
Doltgres expectations are preserved and those scenarios are not counted as green.
