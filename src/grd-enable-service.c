#include <config.h>

#include <gio/gio.h>
#include <limits.h>
#include <polkit/polkit.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#define GRD_SYSTEMD_SERVICE "gnome-remote-desktop.service"
#define GRD_POLKIT_ACTION "org.gnome.remotedesktop.configure-system-daemon"

static gboolean
check_polkit_permission (const char  *action_id,
                         pid_t        pid,
                         GError     **error)
{
  g_autoptr (GPermission) permission = NULL;
  g_autoptr (PolkitSubject) subject = NULL;

  subject = polkit_unix_process_new_for_owner (pid, 0, -1);
  permission = polkit_permission_new_sync (action_id, subject, NULL, error);
  if (!permission)
    return FALSE;

  if (!g_permission_get_allowed (permission))
    {
      g_set_error (error,
                   G_IO_ERROR,
                   G_IO_ERROR_PERMISSION_DENIED,
                   "This program requires callers to have access to the %s "
                   "polkit action", action_id);
      return FALSE;
    }

  return TRUE;
}

static gboolean
enable_systemd_unit (GError **error)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) reply = NULL;
  const char *unit;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (!connection)
    return FALSE;

  unit = GRD_SYSTEMD_SERVICE;
  reply = g_dbus_connection_call_sync (connection,
                                       "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.systemd1.Manager",
                                       "StartUnit",
                                       g_variant_new ("(ss)",
                                                      unit, "replace"),
                                       NULL,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                       -1, NULL, error);
  if (!reply)
    return FALSE;

  reply = g_dbus_connection_call_sync (connection,
                                       "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.systemd1.Manager",
                                       "EnableUnitFiles",
                                       g_variant_new ("(^asbb)",
                                                      (const char*[])
                                                      { unit, NULL },
                                                      FALSE, FALSE),
                                       (GVariantType *) "(ba(sss))",
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       error);

  return reply != NULL;
}

static gboolean
disable_systemd_unit (GError **error)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) reply = NULL;
  const char *unit;

  connection = g_bus_get_sync (G_BUS_TYPE_SYSTEM, NULL, error);
  if (!connection)
    return FALSE;

  unit = GRD_SYSTEMD_SERVICE;
  reply = g_dbus_connection_call_sync (connection,
                                       "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.systemd1.Manager",
                                       "StopUnit",
                                       g_variant_new ("(ss)",
                                                      unit, "replace"),
                                       NULL,
                                       G_DBUS_CALL_FLAGS_NO_AUTO_START,
                                       -1, NULL, error);
  if (!reply)
    return FALSE;

  reply = g_dbus_connection_call_sync (connection,
                                       "org.freedesktop.systemd1",
                                       "/org/freedesktop/systemd1",
                                       "org.freedesktop.systemd1.Manager",
                                       "DisableUnitFiles",
                                       g_variant_new ("(^asb)",
                                                      (const char*[])
                                                      { unit, NULL },
                                                      FALSE),
                                       (GVariantType *) "(a(sss))",
                                       G_DBUS_CALL_FLAGS_NONE,
                                       -1,
                                       NULL,
                                       error);

  return reply != NULL;
}

static void
print_usage (void)
{
  g_printerr ("Usage: %s pid <true|false>\n", g_get_prgname ());
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr (GError) error = NULL;
  gboolean success;
  gboolean enable;
  int64_t pid;

  g_set_prgname (argv[0]);

  if (argc != 3)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  success = g_ascii_string_to_signed (argv[1], 10, 1, INT_MAX, &pid, NULL);
  if (!success)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  enable = g_strcmp0 (argv[2], "true") == 0;
  if (!enable && g_strcmp0 (argv[2], "false") != 0)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  if (geteuid () != 0)
    {
      g_printerr ("This program is not meant to be run directly by users.\n");
      return EXIT_FAILURE;
    }

  if (!check_polkit_permission (GRD_POLKIT_ACTION, (pid_t) pid, &error))
    {
      g_printerr ("Authorization failed for " GRD_POLKIT_ACTION ": %s\n",
                  error->message);
      return EXIT_FAILURE;
    }

  if (enable)
    success = enable_systemd_unit (&error);
  else
    success = disable_systemd_unit (&error);
  if (!success)
    {
      g_printerr ("Failed to %s " GRD_SYSTEMD_SERVICE ": %s\n",
                  enable ? "enable" : "disable", error->message);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
