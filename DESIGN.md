# Project Goals

We have forked Postgres and will be replacing its tuple and index storage with a Prolly Tree. Our
reference for a Prolly Tree is the Dolt project in `workspace/dolthub/dolt`. The prolly tree itself
is implemented mostly in the `store` and `prolly` directories in that project.

The goal of this fork is to retain all the features and compatibility of existing Postgres,
including wire format and protocol, durability, functions, types, query planning, transaction
capabilities, locking, isolation levels, replication (to other instances built from this fork),
while adding the dolt version control capabilities made possible by a Prolly Tree:

* Version control of tables for both data and schema
* Multiple branch heads on a single database, editable concurrently by different sessions
* Efficient diff between two revisions or branches of any table
* Merging one branch into another via `dolt_merge()`.
* Dolt system tables and table functions, such as `dolt_diff()`, `dolt_log`, etc.
* Remote operations such as clone, push, pull.

Our goal is to replace just enough of Postgres to achieve these goals, while leaving as much of the
non-storage-related code in place as possible to make it possible to pull future release from our
upstream.

# Prolly Tree features and requirements

The Prolly Tree is the most important data structure in this project. We don't need to exactly copy
Dolt's implementation, but it has several features which must be reproduced for this project to be
successful:

* Content addressed. Each node in the tree is addressed by a computed hash of its contents. Internal
  nodes in the tree name children by these hashes as well.
* History independence. Two tables with the same logical contents must be considered equal by the
  system and must hash to the same value, regardless of the sequence of statements and transactions
  that gave them those contents.
* Structural sharing. A small edit to a prolly tree creates a spline of edited nodes from one or
  more leaf nodes back to the root. But most of the tree is unaffected, and those chunks with
  idential addresses are shared by every tree that contains a reference to them.

# Other version control requirements

There are also additional requirements that aren't strictly related to prolly trees but are required
to make the version control features work, as learned by Dolt over the last 8 years of development.

* Schema and data versioned together. A single atomic change can change both schema and data.
* Commits are stored in a graph that records the parents of each commit, similar to git. Merge
  commits have 2 or more parents.
* A commit records the set of tables, their data rows, their schemas, and other storage elements
  necesary to reproduce an entire database as it exists at a particular commit. Other storage
  elements include triggers, namespaces (whether they have tables or not), stored procedures,
  foreign key and other contstraint definitions, sequences, etc.
* A global manifest for each database records the set of refs for the database, and this manifest is
  updated when a transaction is committed. These refs include things like branch heads as in git,
  but also things like tags, as well as working sets, which represent the set of durably persisted
  changes that haven't yet been committed to a branch head. Each branch head has its own working
  set. Each ref contains a content-addressed hash that points to its contents (a commit, a tag, a
  working set, etc.)
  
Dolt implements extension to standard SQL both in terms of syntax and semantics. Here are some of
the major ones:

* Each session connects to a particular branch of the database. A session can change the currently
  selected branch with `dolt_checkout()`. A table name in a query resolves to a particular branch
  during planning, depending on the sessions's branch.
* Each branch or other ref can be referenced in a query with special syntax, like the following:
  SELECT * FROM "mydb/myBranch".public.myTable. This query returns rows from the table as it exists
  on the `myBranch` branch, regardless of the session's checked out branch. "mydb@myBranch" is a
  supported alias for this syntax.
* It is possible to atomically commit changes to N different working sets or branch heads in the
  same SQL transaction.
* Some statements, like `SELECT dolt_commit(...)`, implicitly commit the SQL transaction.

# Implications for integrating prolly tree storage into Postgres

There are too many to list and we'll discover more as we write this. But here are a few standouts:

* OIDs are used by the pg_catalog tables to refer to different schema entities. Because of history
  independence, these cannot be assigned durably or sequentially, and must instead be computed as
  needed based on the identity of a particular entity. (OIDs of built-in system entities can and
  must remain the same, as they are hard-coded into many libraries and tools). An OID returned to
  clients is guaranteed to be stable inside of a single transaction, but not otherwise.
* Sequences manage their state globally, so that all branches with a sequence of the same name and
  definition share the same globally consistent counter.
* System metadata used to implement transaction, durability, and visibility requirements must not be
  interleaved with customer data, or otherwise be reflected in data hashes. Encoding any such
  information into the main data prolly tree would violate history independence, since different
  sequences of statements and transactions that produce logically identical data would have
  different binary contents due to the differing system metadata columns. Therefore such information
  must be stored out of band in some way.
