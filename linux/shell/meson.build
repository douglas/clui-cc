project('clui-shell', 'c',
  version: '0.1.0',
  default_options: ['warning_level=2', 'c_std=c17'],
)

gtk4 = dependency('gtk4', version: '>= 4.12')
layer_shell = dependency('gtk4-layer-shell-0', version: '>= 1.0')
webkitgtk = dependency('webkitgtk-6.0', version: '>= 2.42')
json_glib = dependency('json-glib-1.0')

executable('clui-shell',
  'clui-shell.c',
  dependencies: [gtk4, layer_shell, webkitgtk, json_glib],
  install: true,
  install_dir: get_option('bindir'),
)

executable('webview-test',
  'webview-test.c',
  dependencies: [gtk4, layer_shell, webkitgtk],
)
