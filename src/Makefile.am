NULL =

BUILT_SOURCES =

libexec_PROGRAMS = gnome-remote-desktop-daemon

noinst_PROGRAMS = gnome-remote-desktop-control

dbus_remote_desktop_built_sources = grd-dbus-remote-desktop.c grd-dbus-remote-desktop.h

$(dbus_remote_desktop_built_sources) : Makefile.am org.gnome.Mutter.RemoteDesktop.xml
	$(AM_V_GEN)gdbus-codegen							\
		--interface-prefix org.gnome.Mutter					\
		--c-namespace GrdDBus							\
		--generate-c-code grd-dbus-remote-desktop				\
		$(srcdir)/org.gnome.Mutter.RemoteDesktop.xml

BUILT_SOURCES += $(dbus_remote_desktop_built_sources)

dbus_screen_cast_built_sources = grd-dbus-screen-cast.c grd-dbus-screen-cast.h

$(dbus_screen_cast_built_sources) : Makefile.am org.gnome.Mutter.ScreenCast.xml
	$(AM_V_GEN)gdbus-codegen							\
		--interface-prefix org.gnome.Mutter					\
		--c-namespace GrdDBus							\
		--generate-c-code grd-dbus-screen-cast					\
		$(srcdir)/org.gnome.Mutter.ScreenCast.xml

BUILT_SOURCES += $(dbus_screen_cast_built_sources)

dbusservice_DATA = org.gnome.RemoteDesktop.service

gnome_remote_desktop_daemon_SOURCES = \
	grd-context.c \
	grd-context.h \
	grd-daemon.c \
	grd-daemon.h \
	grd-pipewire-stream.c \
	grd-pipewire-stream.h \
	grd-pipewire-stream-monitor.c \
	grd-pipewire-stream-monitor.h \
	grd-private.h \
	grd-session.c \
	grd-session.h \
	grd-session-dummy.c \
	grd-session-dummy.h \
	grd-session-vnc.c \
	grd-session-vnc.h \
	grd-stream.c \
	grd-stream.h \
	grd-types.h \
	grd-vnc-server.c \
	grd-vnc-server.h \
	grd-vnc-sink.c \
	grd-vnc-sink.h \
	$(NULL)

gnome_remote_desktop_daemon_SOURCES : $(BUILT_SOURCES)

nodist_gnome_remote_desktop_daemon_SOURCES = \
	$(BUILT_SOURCES) \
	$(NULL)

gnome_remote_desktop_daemon_CFLAGS = \
	$(BASE_DEPS_CFLAGS) \
	$(AM_CFLAGS)

gnome_remote_desktop_daemon_LDFLAGS = \
	$(BASE_DEPS_LDFLAGS) \
	$(AM_LDFLAGS)

gnome_remote_desktop_daemon_LDADD = \
	$(BASE_DEPS_LIBS)

gnome_remote_desktop_control_SOURCES = \
	grd-control.c \
	$(NULL)

gnome_remote_desktop_control_CFLAGS = \
	$(BASE_DEPS_CFLAGS) \
	$(AM_CFLAGS)

gnome_remote_desktop_control_LDFLAGS = \
	$(BASE_DEPS_LDFLAGS) \
	$(AM_LDFLAGS)

gnome_remote_desktop_control_LDADD = \
	$(BASE_DEPS_LIBS)
