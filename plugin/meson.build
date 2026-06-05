# gedit looks up plugins under XDG_DATA_HOME first, so install to
# ~/.local/share/gedit/plugins/llmghost/ regardless of the meson prefix.
# Nothing else in the project is install:true, so this only affects the
# plugin module + descriptor.
home_dir = run_command('sh', '-c', 'echo "$HOME"', check: true).stdout().strip()
plugin_install_dir = home_dir / '.local' / 'share' / 'gedit' / 'plugins' / 'llmghost'

shared_module(
  'llmghost',                       # produces libllmghost.so to match Module=llmghost
  'llmghost-plugin.c',
  'llmghost-configurable.c',
  dependencies: [
    llmghost_dep,
    gedit_dep,
    peas_dep,
    peas_gtk_dep,
    gtk_dep,
    glib_dep,
    gobject_dep,
  ],
  install: true,
  install_dir: plugin_install_dir,
  name_prefix: 'lib',
)

install_data(
  'llmghost.plugin',
  install_dir: plugin_install_dir,
)
