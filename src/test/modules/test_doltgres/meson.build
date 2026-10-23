# Phase 1 branch catalog experiment.
test_doltgres_sources = files('test_doltgres.c', 'catalog_view.c')
if host_system == 'windows'
  test_doltgres_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'test_doltgres',
    '--FILEDESC', 'test_doltgres - branch catalog projection experiment',])
endif

test_doltgres = shared_module('test_doltgres', test_doltgres_sources,
  kwargs: pg_test_mod_args)
test_install_libs += test_doltgres

# Keep allocation tests separate from the old projection fixture's _PG_init hook.
test_storage_resource = shared_module('storage_resource', files('storage_resource.c'),
  kwargs: pg_test_mod_args)
test_install_libs += test_storage_resource

tests += {
  'name': 'test_doltgres',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {'sql': ['branch_catalog'], 'dbname': 'contrib_regression'},
  'tap': {'tests': ['t/001_branch_catalog.pl', 't/002_catalog_views.pl', 't/003_storage_allocation.pl', 't/004_storage_writes.pl', 't/005_canonical_sql.pl', 't/006_canonical_merge.pl', 't/007_canonical_alter.pl', 't/008_catalog_context.pl', 't/009_revision_views.pl', 't/010_search_path.pl', 't/011_nested_resolution.pl', 't/012_execution_context.pl', 't/013_routines.pl', 't/014_dependencies.pl', 't/015_native_mutation.pl', 't/016_lifecycle.pl', 't/017_provisional_ddl.pl', 't/018_index_lifecycle.pl', 't/019_foreign_keys.pl']},
}
