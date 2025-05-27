#include <config.h>

#include <gio/gio.h>
#include <limits.h>
#include <polkit/polkit.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>

#include "grd-enums.h"

#define GRD_SYSTEMD_SERVICE "gnome-remote-desktop.service"
#define GRD_SYSTEMD_HEADLESS_SERVICE "gnome-remote-desktop-headless.service"
#define GRD_POLKIT_ACTION "org.gnome.remotedesktop.configure-system-daemon"

static gboolean
get_runtime_mode (const char     *argv,
                  GrdRuntimeMode *runtime_mode)
{
  if (g_str_equal (argv, "user"))
    *runtime_mode = GRD_RUNTIME_MODE_SCREEN_SHARE;
  else if (g_str_equal (argv, "headless"))
    *runtime_mode = GRD_RUNTIME_MODE_HEADLESS;
  else if (g_str_equal (argv, "system"))
    *runtime_mode = GRD_RUNTIME_MODE_SYSTEM;
  else
    return FALSE;

  return TRUE;
}

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
validate_caller (int64_t pid)
{
  g_autoptr (GError) error = NULL;

  if (geteuid () != 0)
    {
      g_printerr ("Enabling system service is not meant to be done directly by users.\n");
      return FALSE;
    }

  if (!check_polkit_permission (GRD_POLKIT_ACTION, (pid_t) pid, &error))
    {
      g_printerr ("Authorization failed for " GRD_POLKIT_ACTION ": %s\n",
                  error->message);
      return FALSE;
    }

  return TRUE;
}

static void
get_systemd_unit_info (GrdRuntimeMode   runtime_mode,
                       GBusType        *bus_type,
                       const char     **unit)
{
  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_HEADLESS:
      *bus_type = G_BUS_TYPE_SESSION;
      *unit = GRD_SYSTEMD_HEADLESS_SERVICE;
      break;
    case GRD_RUNTIME_MODE_SYSTEM:
      *bus_type = G_BUS_TYPE_SYSTEM;
      *unit = GRD_SYSTEMD_SERVICE;
      break;
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
      *bus_type = G_BUS_TYPE_SESSION;
      *unit = GRD_SYSTEMD_SERVICE;
      break;
    default:
      g_assert_not_reached ();
    }
}

static gboolean
enable_systemd_unit (GrdRuntimeMode   runtime_mode,
                     GError         **error)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) reply = NULL;
  GBusType bus_type;
  const char *unit;

  get_systemd_unit_info (runtime_mode, &bus_type, &unit);

  connection = g_bus_get_sync (bus_type, NULL, error);
  if (!connection)
    return FALSE;

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
disable_systemd_unit (GrdRuntimeMode   runtime_mode,
                      GError         **error)
{
  g_autoptr (GDBusConnection) connection = NULL;
  g_autoptr (GVariant) reply = NULL;
  GBusType bus_type;
  const char *unit;

  get_systemd_unit_info (runtime_mode, &bus_type, &unit);

  connection = g_bus_get_sync (bus_type, NULL, error);
  if (!connection)
    return FALSE;

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
  g_printerr ("Usage: %s pid <user|headless|system> <true|false>\n", g_get_prgname ());
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr (GError) error = NULL;
  GrdRuntimeMode runtime_mode;
  gboolean success;
  gboolean enable;
  int64_t pid;

  g_set_prgname (argv[0]);

  if (argc != 4)
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

  success = get_runtime_mode (argv[2], &runtime_mode);
  if (!success)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  enable = g_strcmp0 (argv[3], "true") == 0;
  if (!enable && g_strcmp0 (argv[3], "false") != 0)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  if (runtime_mode == GRD_RUNTIME_MODE_SYSTEM)
    {
      success = validate_caller (pid);
      if (!success)
        return EXIT_FAILURE;
    }

  if (enable)
    success = enable_systemd_unit (runtime_mode, &error);
  else
    success = disable_systemd_unit (runtime_mode, &error);
  if (!success)
    {
      g_printerr ("Failed to %s service: %s\n",
                  enable ? "enable" : "disable", error->message);
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}
