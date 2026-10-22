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

tests += {
  'name': 'test_doltgres',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {'sql': ['branch_catalog'], 'dbname': 'contrib_regression'},
  'tap': {'tests': ['t/001_branch_catalog.pl', 't/002_catalog_views.pl']},
}
