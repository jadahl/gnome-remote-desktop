daemon_sources = files([
  'grd-context.c',
  'grd-context.h',
  'grd-daemon.c',
  'grd-daemon.h',
  'grd-private.h',
  'grd-session.c',
  'grd-session.h',
  'grd-session-dummy.c',
  'grd-session-dummy.h',
  'grd-session-vnc.c',
  'grd-session-vnc.h',
  'grd-stream.c',
  'grd-stream.h',
  'grd-types.h',
  'grd-vnc-server.c',
  'grd-vnc-server.h',
  'grd-vnc-sink.c',
  'grd-vnc-sink.h',
])

gen_daemon_sources = []

gen_daemon_sources += gnome.gdbus_codegen('grd-dbus-screen-cast',
                                          'org.gnome.Mutter.ScreenCast.xml',
                                          interface_prefix: 'org.gnome.Mutter.',
                                          namespace: 'GrdDBus')
gen_daemon_sources += gnome.gdbus_codegen('grd-dbus-remote-desktop',
                                          'org.gnome.Mutter.RemoteDesktop.xml',
                                          interface_prefix: 'org.gnome.Mutter.',
                                          namespace: 'GrdDBus')

daemon_sources += gen_daemon_sources

control_sources = ([
  'grd-control.c'
])

executable('gnome-remote-desktop-daemon',
           daemon_sources,
           dependencies: [glib_dep,
                          gio_dep,
                          gio_unix_dep,
                          pipewire_dep,
                          gstreamer_dep,
                          libvncserver_dep],
           include_directories: [configinc],
           install : true)

executable('gnome-remote-desktop-control',
           control_sources,
           dependencies: [glib_dep, gio_dep],
           include_directories: [configinc],
           install : false)
