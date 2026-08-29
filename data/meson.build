# SPDX-License-Identifier: GPL-3.0-or-later

m42_resources = gnome.compile_resources(
  'm42-resources',
  'math42.gresource.xml',
  source_dir: meson.current_source_dir(),
  c_name: 'm42',
)

install_data('net.office42.math42.desktop',
  install_dir: get_option('datadir') / 'applications')
install_data('icons/scalable/apps/net.office42.math42.svg',
  install_dir: get_option('datadir') / 'icons/hicolor/scalable/apps')
