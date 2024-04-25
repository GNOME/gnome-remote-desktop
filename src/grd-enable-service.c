#include <gio/gio.h>
#include <stdlib.h>
#include <stdio.h>
#include <polkit/polkit.h>

#define GRD_SYSTEMD_SERVICE "gnome-remote-desktop.service"
#define GRD_POLKIT_ACTION "org.gnome.remotedesktop.enable-system-daemon"

static gboolean
check_polkit_permission (const char  *action_id,
                         GError     **error)
{
  g_autoptr (PolkitPermission) permission = NULL;

  permission = polkit_permission_new_sync (action_id, NULL, NULL, error);
  if (!permission)
    return FALSE;

  return g_permission_get_allowed (G_PERMISSION (permission));
}

static gboolean
enable_systemd_unit (GError **error)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) reply = NULL;
  const char *unit,

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
  const char *unit,

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

int
main (int  argc,
      char *argv[])
{
  g_autoptr (GError) error = NULL;
  gboolean enable;
  gboolean success;

  if (argc != 2)
    {
      g_printerr ("Usage: %s <true|false>\n", argv[0]);
      return EXIT_FAILURE;
    }

  enable = g_strcmp0 (argv[1], "true") == 0;
  if (!enable && g_strcmp0 (argv[1], "false") != 0)
    {
      g_printerr ("Usage: %s <true|false>\n", argv[0]);
      return EXIT_FAILURE;
    }

  if (!check_polkit_permission (GRD_POLKIT_ACTION, &error))
    {
      g_printerr ("Authorization failed for " GRD_POLKIT_ACTION ": %s\n",
                  error->message);
      return EXIT_FAILURE;
    }

  if (enable)
    {
      success = enable_systemd_unit (&error);
    }
  else
    {
      success = disable_systemd_unit (&error);
    }

  if (!success)
    {
      g_printerr ("Failed to %s " GRD_SYSTEMD_SERVICE ": %s\n",
                  enable ? "enable" : "disable", error->message);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
