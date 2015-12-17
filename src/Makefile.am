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

dbusservice_DATA = org.gnome.RemoteDesktop.service

gnome_remote_desktop_daemon_SOURCES = \
	grd-context.c \
	grd-context.h \
	grd-daemon.c \
	grd-daemon.h \
	grd-private.h \
	grd-session.c \
	grd-session.h \
	grd-session-dummy.c \
	grd-session-dummy.h \
	grd-types.h \
	$(NULL)

gnome_remote_desktop_daemon_SOURCES : $(BUILT_SOURCES)

nodist_gnome_remote_desktop_daemon_SOURCES = \
	$(dbus_remote_desktop_built_sources) \
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
