tests += {
  'name': 'test_prolly',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {'sql': ['storage']},
  'tap': {'tests': [
    't/001_recovery.pl',
    't/002_differential.pl',
    't/003_types.pl',
    't/004_portability.pl',
    't/005_native.pl',
    't/006_manifest.pl',
    't/007_commands.pl',
    't/008_publication.pl',
  ]},
}
