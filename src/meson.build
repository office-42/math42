# SPDX-License-Identifier: GPL-3.0-or-later

m42_inc = include_directories('core', 'ui')

# The language, below the user interface.  Its own library, linking
# without GTK, so that nothing graphical can reach into it.
m42_core_sources = files(
  'core/m42-bigint.c',
  'core/m42-pattern.c',
  'core/m42-format.c',
  'core/m42-mat.c',
  'core/m42-value.c',
  'core/m42-lexer.c',
  'core/m42-parser.c',
  'core/m42-symbolic.c',
  'core/m42-help.c',
  'core/m42-matrix.c',
  'core/m42-eval.c',
)

m42_core = static_library('m42core',
  m42_core_sources,
  dependencies: core_deps,
  include_directories: m42_inc,
  install: false,
)

m42_core_dep = declare_dependency(
  link_with: m42_core,
  include_directories: m42_inc,
  dependencies: core_deps,
)

# The engine can be driven from a terminal: one expression per line in,
# one result per line out.  It is how CI checks the engine on a runner
# without a display.
executable('math42-calc',
  'calc-main.c',
  dependencies: [m42_core_dep],
  install: false,
)

m42_ui_sources = files(
  'ui/m42-typeset.c',
  'ui/m42-notebook.c',
  'ui/m42-window.c',
  'ui/m42-application.c',
  'main.c',
)

executable('math42',
  m42_ui_sources,
  m42_resources,
  dependencies: [m42_core_dep] + app_deps,
  include_directories: m42_inc,
  install: true,
  win_subsystem: 'windows',
)
