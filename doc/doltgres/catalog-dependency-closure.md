# Catalog milestone 3: native dependency closure

Canonical deletion now calls PostgreSQL's existing findDependentObjects and
reportDependentObjects through collect_catalog_deletions. It uses the same
NORMAL/AUTO/INTERNAL edge rules, internal ownership protection, cycle handling,
multi-target union and RESTRICT/CASCADE diagnostics as native catalog deletion.
The new entry point returns the validated object list without deleting physical
catalog rows. The caller holds the branch working-set lock across collection and
publication. Runtime catalog handles are never used as cross-backend lock keys.

Immutable-view systable scans now support dependency visibility rechecks: their
pinned rows cannot change while a lock is acquired. Ordinary physical scans retain
their native snapshot recheck. Both recursive lock paths preserve the contextual
collection flag. A physical deletion path receiving a canonical handle fails
explicitly rather than using that handle as a physical identity.

The publisher removes selected namespace/table/index/routine members together.
Generated row types, arrays and constraints disappear by regeneration from the
new canonical root. Removing a standalone index or a constraint updates the
surviving table's index/constraint metadata and retains its storage resource and
captured data root. DROP of a constraint-owned index still fails with PostgreSQL's
native ownership diagnostic. ALTER TABLE DROP CONSTRAINT uses the same closure;
not-null removal also reuses an extracted native validation function, preserving
primary-key, replica-identity and identity-column restrictions.

All changes publish as one working-set ref update in the SQL transaction. Old
catalog roots, tuples and retained chunks remain reachable for another branch,
old SQL snapshot, cursor or committed revision. Rollback restores the complete
closure. Another session on a different branch can keep using its prepared plan
and write while a CASCADE transaction remains open.

## Scope and remaining work

Supported deletion roots are canonical tables, indexes, schemas, functions and
constraints. Direct DROP of a generated table row type encounters native internal
ownership protection. Unsupported dependency object classes and column-level
closure entries reject before publication. Foreign keys, stored views, parsed
routine bodies, defaults and expression indexes still need canonical definition
support before their dependency edges can be supplied. The traversal is general;
this milestone does not claim those definitions or their DDL are implemented.
Mixed CASCADE/RESTRICT behaviors in one ALTER TABLE, concurrent DROP and event
triggers remain rejected. Native sequential ALTER semantics beyond supported
metadata commands require a broader provisional-catalog mutation interface.

Roles, extensions and other shared objects are not versioned here; this does not
complete cross-database shared-dependency/role-lifetime integration. Relation
storage and immutable history are retained, not physically garbage-collected by
DROP. Reclamation belongs to the lifecycle gate.

## Verification and integration fixes

The new native dependency test exercises direct/index/array dependencies,
RESTRICT, ownership protection, CASCADE, constraint removal and uniqueness,
rollback, other-branch prepared queries/writes, historical reads and restart.
Existing native PostgreSQL regression expected outputs remain unchanged. The old
prototype test that expected every CASCADE to be unsupported now checks successful
CASCADE followed by rollback instead.

The full native suite caught an eager catalog-activation check acquiring a
snapshot for SET TRANSACTION; dispatch now filters statement kinds first. The
existing owner/temp-shadow test found that temporary-namespace candidates were
being looked up in an immutable catalog view. Resolution now obtains canonical
candidates from its explicit root and shared/temp candidates from an explicit
physical scope, restoring the prior scope on errors.

Doltgres coverage includes the original function-parameter dependency test and
new branch-local routine/index/constraint deletion cases. Its harness includes
PostgreSQL's structured Detail in diagnostic comparisons, preserving the original
Message and SQLSTATE rather than weakening multiline dependency assertions.
