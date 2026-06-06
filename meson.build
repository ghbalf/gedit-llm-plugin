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
secret_dep  = dependency('libsecret-1', version: '>= 0.20')
gedit_dep   = dependency('gedit',        version: '>= 46')
peas_dep     = dependency('libpeas-1.0',     version: '>= 1.20')
peas_gtk_dep = dependency('libpeas-gtk-1.0', version: '>= 1.20')

# Headless GTK tests run under a virtual framebuffer. We wrap only the gui
# test in xvfb-run (see tests/meson.build) rather than making it a default
# test setup, so the display-free unit suite stays dependency-light and pays
# no xvfb overhead. The gui test is only registered when xvfb-run is present.
xvfb_run = find_program('xvfb-run', required: false)

llmghost_inc = include_directories('lib')

subdir('lib')
subdir('tests')
subdir('plugin')
