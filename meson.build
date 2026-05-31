project('gedit-llm-plugin', 'c',
  version: '0.1.0',
  license: 'GPL-3.0-or-later',
  meson_version: '>= 1.0',
  default_options: [
    'c_std=gnu11',
    'warning_level=2',
    'buildtype=debugoptimized',
  ],
)

cc = meson.get_compiler('c')

gtk_dep     = dependency('gtk+-3.0',     version: '>= 3.24')
glib_dep    = dependency('glib-2.0',     version: '>= 2.74')
gobject_dep = dependency('gobject-2.0')
gio_dep     = dependency('gio-2.0')
soup_dep    = dependency('libsoup-3.0',  version: '>= 3.0')
json_dep    = dependency('json-glib-1.0', version: '>= 1.6')
gedit_dep   = dependency('gedit',        version: '>= 46')
peas_dep    = dependency('libpeas-1.0',  version: '>= 1.20')

llmghost_inc = include_directories('lib')

subdir('lib')
subdir('tests')
subdir('plugin')
