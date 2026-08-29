# math42 - a computer algebra and numerics notebook, in the shape of
# Mathematica and MATLAB
# Copyright (C) 2026 The math42 authors
# SPDX-License-Identifier: GPL-3.0-or-later

project('math42', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=gnu11,c11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')
gnome = import('gnome')

add_project_arguments(
  '-DM42_VERSION="' + meson.project_version() + '"',
  '-DG_LOG_DOMAIN="math42"',
  language: 'c',
)

extra_warnings = [
  '-Wmissing-prototypes',
  '-Wstrict-prototypes',
  '-Wold-style-definition',
  '-Wpointer-arith',
  '-Wshadow',
  '-Wwrite-strings',
  '-Wformat=2',
  '-Wundef',
  '-Wvla',
  '-Winit-self',
  '-Wno-unused-parameter',
]
add_project_arguments(cc.get_supported_arguments(extra_warnings), language: 'c')

glib_dep       = dependency('glib-2.0',    version: '>= 2.76')
gobject_dep    = dependency('gobject-2.0', version: '>= 2.76')
gio_dep        = dependency('gio-2.0',     version: '>= 2.76')
cairo_dep      = dependency('cairo',       version: '>= 1.16')
pango_dep      = dependency('pango',       version: '>= 1.50')
pangocairo_dep = dependency('pangocairo',  version: '>= 1.50')
gtk_dep        = dependency('gtk4',        version: '>= 4.10')
m_dep          = cc.find_library('m', required: false)

# The core is the language: values, the parser and the evaluator.  It
# needs GLib and libm and nothing graphical, and must stay that way.
core_deps = [glib_dep, gobject_dep, gio_dep, m_dep]
app_deps  = core_deps + [cairo_dep, pango_dep, pangocairo_dep, gtk_dep]

subdir('data')
subdir('src')

summary({
    'prefix' : get_option('prefix'),
    'GTK'    : gtk_dep.version(),
    'Pango'  : pango_dep.version(),
  }, section: 'math42 ' + meson.project_version())
