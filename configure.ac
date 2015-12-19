AC_PREREQ([2.63])
AC_INIT([gnome-remote-desktop], [0.1], [jadahl@gmail.com])
AC_CONFIG_HEADER([config.h])
AC_CONFIG_MACRO_DIR([m4])
AC_CONFIG_AUX_DIR([build-aux])

AM_INIT_AUTOMAKE([1.13 foreign no-define tar-ustar no-dist-gzip dist-xz subdir-objects])
AM_MAINTAINER_MODE([enable])
AM_SILENT_RULES([yes])
AC_USE_SYSTEM_EXTENSIONS
AC_SYS_LARGEFILE

AC_PROG_CC
AM_PROG_CC_C_O
AM_PROG_LIBTOOL

GETTEXT_PACKAGE=gnome-remote-desktop
AC_SUBST(GETTEXT_PACKAGE)
AC_DEFINE_UNQUOTED(GETTEXT_PACKAGE, "$GETTEXT_PACKAGE",
		   [The prefix for our gettext translation domains.])

AC_ARG_WITH([dbus-service-dir],
	    [AS_HELP_STRING([--with-dbus-service-dir=PATH],[dbus service file directory])],
	    [dbusservicedir="$withval"],
	    [dbusservicedir='${datadir}/dbus-1/services'])
AC_SUBST([dbusservicedir])

PKG_CHECK_MODULES(BASE_DEPS, glib-2.0 gio-unix-2.0 libpinos-0.1 gstreamer-1.0 >= 1.6)

AX_APPEND_COMPILE_FLAGS([-Wall -Wstrict-prototypes -Werror=missing-prototypes \
			 -Werror=implicit-function-declaration \
			 -Werror=pointer-arith -Werror=init-self -Werror=format=2 \
			 -Werror=format-security \
			 -Werror=missing-include-dirs -Werror=aggregate-return \
			 -Werror=declaration-after-statement])

AC_OUTPUT([
Makefile
src/Makefile
src/org.gnome.RemoteDesktop.service
])

echo "
                      gnome remote desktop service
		      ============================

        prefix:                         ${prefix}
"
