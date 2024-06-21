/*
 * Copyright (C) 2021-2022 Red Hat Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 *
 */

#include "config.h"

#include <ctype.h>
#include <gio/gio.h>
#include <gio/gunixinputstream.h>
#include <gio/gunixoutputstream.h>
#include <glib/gi18n.h>
#include <pwd.h>
#include <stdint.h>
#include <stdio.h>
#include <sysexits.h>
#include <termios.h>
#include <unistd.h>

#include "grd-enums.h"
#include "grd-settings-system.h"
#include "grd-settings-user.h"
#include "grd-utils.h"

#define GRD_SYSTEMD_SERVICE "gnome-remote-desktop.service"

typedef enum
{
  PROMPT_TYPE_VISIBLE_TEXT,
  PROMPT_TYPE_HIDDEN_TEXT
} PromptType;

typedef struct _SubCommand
{
  const char *subcommand;
  gboolean (* process) (GrdSettings  *settings,
                        int           argc,
                        char        **argv,
                        GError      **error);
  int n_args;
} SubCommand;

static void
log_handler (const char     *log_domain,
             GLogLevelFlags  log_level,
             const char     *message,
             gpointer        user_data)
{
  fprintf (stderr, "%s.\n", message);
}

static void
print_usage (void)
{
  printf (_("Usage: %s [OPTIONS...] COMMAND [SUBCOMMAND]...\n"),
          g_get_prgname ());
}

static int
process_options (GrdSettings       *settings,
                 int                argc,
                 char             **argv,
                 const SubCommand  *subcommands,
                 int                n_subcommands)
{
  gboolean subcommand_exists = FALSE;
  int i;

  if (argc <= 0)
    return EX_USAGE;

  for (i = 0; i < n_subcommands; i++)
    {
      g_autoptr (GError) error = NULL;

      if (g_strcmp0 (argv[0], subcommands[i].subcommand) != 0)
        continue;

      subcommand_exists = TRUE;

      if (subcommands[i].n_args != argc - 1)
        continue;

      if (!subcommands[i].process (settings,
                                   argc - 1, argv + 1, &error))
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }

      if (!GRD_IS_SETTINGS_SYSTEM (settings))
        g_settings_sync ();

      return EXIT_SUCCESS;
    }

  if (subcommand_exists)
    g_printerr ("Wrong number of arguments for subcommand '%s'\n", argv[0]);
  else
    g_printerr ("Unknown subcommand '%s'\n", argv[0]);

  return EX_USAGE;
}

static gboolean
is_numeric (const char *s)
{
  while (*s)
    {
      if (isdigit (*s++) == 0)
        return FALSE;
    }

  return TRUE;
}

#ifdef HAVE_RDP
static gboolean
rdp_set_port (GrdSettings  *settings,
              int           argc,
              char        **argv,
              GError      **error)
{
  int port;

  if (!is_numeric (argv[0]))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "The port must be an integer");
      return FALSE;
    }

  port = strtol (argv[0], NULL, 10);
  g_object_set (G_OBJECT (settings), "rdp-port", port, NULL);
  return TRUE;
}

static gboolean
rdp_enable (GrdSettings  *settings,
            int           argc,
            char        **argv,
            GError      **error)
{
  if (GRD_IS_SETTINGS_SYSTEM (settings))
    {
      if (!grd_toggle_systemd_unit (TRUE, error))
        return FALSE;
    }

  g_object_set (G_OBJECT (settings), "rdp-enabled", TRUE, NULL);

  return TRUE;
}

static gboolean
rdp_disable (GrdSettings  *settings,
             int           argc,
             char        **argv,
             GError      **error)
{
  if (GRD_IS_SETTINGS_SYSTEM (settings))
    {
      if (!grd_toggle_systemd_unit (FALSE, error))
        return FALSE;
    }

  g_object_set (G_OBJECT (settings), "rdp-enabled", FALSE, NULL);

  return TRUE;
}

static gboolean
rdp_set_tls_cert (GrdSettings  *settings,
                  int           argc,
                  char        **argv,
                  GError      **error)
{
  char *path = argv[0];

  if (!g_path_is_absolute (path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Path to certificate file not absolute");
      return FALSE;
    }

  g_object_set (G_OBJECT (settings), "rdp-server-cert-path", argv[0], NULL);
  return TRUE;
}

static gboolean
rdp_set_tls_key (GrdSettings  *settings,
                 int           argc,
                 char        **argv,
                 GError      **error)
{
  char *path = argv[0];

  if (!g_path_is_absolute (path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Path to certificate file not absolute");
      return FALSE;
    }

  g_object_set (G_OBJECT (settings), "rdp-server-key-path", argv[0], NULL);
  return TRUE;
}

static char *
prompt_for_input (const char  *prompt,
                  PromptType   prompt_type,
                  GError     **error)
{
  g_autoptr (GInputStream) input_stream = NULL;
  g_autoptr (GOutputStream) output_stream = NULL;
  g_autoptr (GDataInputStream) data_input_stream = NULL;
  gboolean terminal_attributes_changed = FALSE;
  struct termios terminal_attributes;
  g_autofree char *input = NULL;
  gboolean success;
  int output_fd;
  int input_fd;

  input_fd = STDIN_FILENO;
  output_fd = STDOUT_FILENO;

  input_stream = g_unix_input_stream_new (input_fd, FALSE);
  data_input_stream = g_data_input_stream_new (input_stream);

  if (isatty (output_fd))
    output_stream = g_unix_output_stream_new (output_fd, FALSE);

  if (output_stream)
    {
      success = g_output_stream_write_all (output_stream, prompt, strlen (prompt), NULL, NULL, error);
      if (!success)
        return NULL;

      if (prompt_type == PROMPT_TYPE_HIDDEN_TEXT && isatty (input_fd))
        {
          struct termios updated_terminal_attributes;
          int ret;

          ret = tcgetattr (input_fd, &terminal_attributes);
          if (ret < 0)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           "Could not query terminal attributes");
              return NULL;
            }

          updated_terminal_attributes = terminal_attributes;
          updated_terminal_attributes.c_lflag &= ~ECHO;

          ret = tcsetattr (input_fd, TCSANOW, &updated_terminal_attributes);
          if (ret < 0)
            {
              g_set_error (error, G_IO_ERROR, g_io_error_from_errno (errno),
                           "Could not disable input echo in terminal");
              return NULL;
            }

          terminal_attributes_changed = TRUE;
        }
    }

  input = g_data_input_stream_read_line_utf8 (data_input_stream, NULL, NULL, error);

  if (terminal_attributes_changed)
    {
      g_output_stream_write_all (output_stream, "\n", strlen ("\n"), NULL, NULL, NULL);
      tcsetattr (input_fd, TCSANOW, &terminal_attributes);
    }

  return g_steal_pointer (&input);
}

static gboolean
rdp_set_credentials (GrdSettings  *settings,
                     int           argc,
                     char        **argv,
                     GError      **error)
{
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;

  if (argc < 1)
    {
      username = prompt_for_input (_("Username: "), PROMPT_TYPE_VISIBLE_TEXT,
                                   error);
      if (!username)
        return FALSE;
    }
  else
    {
      username = g_strdup (argv[0]);
    }

  if (argc < 2)
    {
      password = prompt_for_input (_("Password: "), PROMPT_TYPE_HIDDEN_TEXT,
                                   error);
      if (!password)
        return FALSE;
    }
  else
    {
      password = g_strdup (argv[1]);
    }

  return grd_settings_set_rdp_credentials (settings, username, password, error);
}

static gboolean
rdp_clear_credentials (GrdSettings  *settings,
                       int           argc,
                       char        **argv,
                       GError      **error)
{
  return grd_settings_clear_rdp_credentials (settings, error);
}

static gboolean
rdp_enable_view_only (GrdSettings  *settings,
                      int           argc,
                      char        **argv,
                      GError      **error)
{
  g_object_set (G_OBJECT (settings), "rdp-view-only", TRUE, NULL);
  return TRUE;
}

static gboolean
rdp_disable_view_only (GrdSettings  *settings,
                       int           argc,
                       char        **argv,
                       GError      **error)
{
  g_object_set (G_OBJECT (settings), "rdp-view-only", FALSE, NULL);
  return TRUE;
}

static gboolean
rdp_enable_port_negotiation (GrdSettings  *settings,
                             int           argc,
                             char        **argv,
                             GError      **error)
{
  g_object_set (G_OBJECT (settings), "rdp-negotiate-port", TRUE, NULL);
  return TRUE;
}

static gboolean
rdp_disable_port_negotiation (GrdSettings  *settings,
                              int           argc,
                              char        **argv,
                              GError      **error)
{
  g_object_set (G_OBJECT (settings), "rdp-negotiate-port", FALSE, NULL);
  return TRUE;
}

static const SubCommand rdp_subcommands[] = {
  { "set-port", rdp_set_port, 1 },
  { "enable", rdp_enable, 0 },
  { "disable", rdp_disable, 0 },
  { "set-tls-cert", rdp_set_tls_cert, 1 },
  { "set-tls-key", rdp_set_tls_key, 1 },
  { "set-credentials", rdp_set_credentials, 0 },
  { "set-credentials", rdp_set_credentials, 1 },
  { "set-credentials", rdp_set_credentials, 2 },
  { "clear-credentials", rdp_clear_credentials, 0 },
  { "enable-view-only", rdp_enable_view_only, 0 },
  { "disable-view-only", rdp_disable_view_only, 0 },
  { "enable-port-negotiation", rdp_enable_port_negotiation, 0 },
  { "disable-port-negotiation", rdp_disable_port_negotiation, 0 },
};

static int
process_rdp_options (GrdSettings  *settings,
                     int           argc,
                     char        **argv)
{
  return process_options (settings,
                          argc, argv,
                          rdp_subcommands,
                          G_N_ELEMENTS (rdp_subcommands));
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static gboolean
vnc_set_port (GrdSettings  *settings,
              int           argc,
              char        **argv,
              GError      **error)
{
  int port;

  if (!is_numeric (argv[0]))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "The port must be an integer");
      return FALSE;
    }

  port = strtol (argv[0], NULL, 10);
  g_object_set (G_OBJECT (settings), "vnc-port", port, NULL);
  return TRUE;
}

static gboolean
vnc_enable (GrdSettings  *settings,
            int           argc,
            char        **argv,
            GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-enabled", TRUE, NULL);
  return TRUE;
}

static gboolean
vnc_disable (GrdSettings  *settings,
             int           argc,
             char        **argv,
             GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-enabled", FALSE, NULL);
  return TRUE;
}

#define MAX_VNC_PASSWORD_SIZE 8

static gboolean
vnc_set_credentials (GrdSettings  *settings,
                     int           argc,
                     char        **argv,
                     GError      **error)
{
  g_autofree char *password = NULL;

  if (argc < 1)
    {
      password = prompt_for_input (_("Password: "), PROMPT_TYPE_HIDDEN_TEXT,
                                   error);
      if (!password)
        return FALSE;
    }
  else
    {
      password = g_strdup (argv[1]);
    }

  if (strlen (password) > MAX_VNC_PASSWORD_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is too long");
      return FALSE;
    }

  return grd_settings_set_vnc_password (settings, password, error);
}

static gboolean
vnc_clear_credentials (GrdSettings  *settings,
                       int           argc,
                       char        **argv,
                       GError      **error)
{
  return grd_settings_clear_vnc_password (settings, error);
}

static gboolean
vnc_set_auth_method (GrdSettings  *settings,
                     int           argc,
                     char        **argv,
                     GError      **error)
{
  char *auth_method = argv[0];

  if (strcmp (auth_method, "prompt") == 0)
    {
      g_object_set (G_OBJECT (settings),
                    "vnc-auth-method", GRD_VNC_AUTH_METHOD_PROMPT,
                    NULL);
      return TRUE;
    }
  else if (strcmp (auth_method, "password") == 0)
    {
      g_object_set (G_OBJECT (settings),
                    "vnc-auth-method", GRD_VNC_AUTH_METHOD_PASSWORD,
                    NULL);
      return TRUE;
    }

  g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
               "Invalid auth method '%s'", auth_method);

  return FALSE;
}

static gboolean
vnc_enable_view_only (GrdSettings  *settings,
                      int           argc,
                      char        **argv,
                      GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-view-only", TRUE, NULL);
  return TRUE;
}

static gboolean
vnc_disable_view_only (GrdSettings  *settings,
                       int           argc,
                       char        **argv,
                       GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-view-only", FALSE, NULL);
  return TRUE;
}

static gboolean
vnc_enable_port_negotiation (GrdSettings  *settings,
                             int           argc,
                             char        **argv,
                             GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-negotiate-port", TRUE, NULL);
  return TRUE;
}

static gboolean
vnc_disable_port_negotiation (GrdSettings  *settings,
                              int           argc,
                              char        **argv,
                              GError      **error)
{
  g_object_set (G_OBJECT (settings), "vnc-negotiate-port", FALSE, NULL);
  return TRUE;
}


static const SubCommand vnc_subcommands[] = {
  { "set-port", vnc_set_port, 1 },
  { "enable", vnc_enable, 0 },
  { "disable", vnc_disable, 0 },
  { "set-password", vnc_set_credentials, 0 },
  { "set-password", vnc_set_credentials, 1 },
  { "clear-password", vnc_clear_credentials, 0 },
  { "set-auth-method", vnc_set_auth_method, 1 },
  { "enable-view-only", vnc_enable_view_only, 0 },
  { "disable-view-only", vnc_disable_view_only, 0 },
  { "enable-port-negotiation", vnc_enable_port_negotiation, 0 },
  { "disable-port-negotiation", vnc_disable_port_negotiation, 0 },
};

static int
process_vnc_options (GrdSettings  *settings,
                     int           argc,
                     char        **argv)
{
  return process_options (settings,
                          argc, argv,
                          vnc_subcommands,
                          G_N_ELEMENTS (vnc_subcommands));
}
#endif /* HAVE_VNC */

static void
print_help (void)
{
  /* For translators: First line of --help after usage line */
  const char *help_header =
    _("Commands:\n");
#ifdef HAVE_RDP
  /* For translators: This first words on each line is the command;
   * don't translate. Try to fit each line within 80 characters. */
  const char *help_rdp =
    _("  rdp                                            - RDP subcommands:\n"
      "    set-port                                     - Set port the server binds to\n"
      "    enable                                       - Enable the RDP backend\n"
      "    disable                                      - Disable the RDP backend\n"
      "    set-tls-cert <path-to-cert>                  - Set path to TLS certificate\n"
      "    set-tls-key <path-to-key>                    - Set path to TLS key\n"
      "    set-credentials [<username> [<password>]]    - Set username and password\n"
      "                                                   credentials\n"
      "    clear-credentials                            - Clear username and password\n"
      "                                                   credentials\n"
      "    enable-view-only                             - Disable remote control of input\n"
      "                                                   devices\n"
      "    disable-view-only                            - Enable remote control of input\n"
      "                                                   devices\n"
      "    enable-port-negotiation                      - If unavailable, listen to\n"
      "                                                   a different port\n"
      "    disable-port-negotiation                     - If unavailable, don't listen\n"
      "                                                   to a different port\n"
      "\n");
#endif /* HAVE_RDP */
#ifdef HAVE_VNC
  /* For translators: This first words on each line is the command;
   * don't translate. Try to fit each line within 80 characters. */
  const char *help_vnc =
    _("  vnc                                        - VNC subcommands:\n"
      "    set-port                                 - Set port the server binds to\n"
      "    enable                                   - Enable the VNC backend\n"
      "    disable                                  - Disable the VNC backend\n"
      "    set-password [<password>]                - Set the VNC password\n"
      "    clear-password                           - Clear the VNC password\n"
      "    set-auth-method password|prompt          - Set the authorization method\n"
      "    enable-view-only                         - Disable remote control of input\n"
      "                                               devices\n"
      "    disable-view-only                        - Enable remote control of input\n"
      "                                               devices\n"
      "    enable-port-negotiation                  - If unavailable, listen to\n"
      "                                               a different port\n"
      "    disable-port-negotiation                 - If unavailable, don't listen\n"
      "                                               to a different port\n"
      "\n");
#endif /* HAVE_VNC */
  /* For translators: This first words on each line is the command;
   * don't translate. Try to fit each line within 80 characters. */
  const char *help_other =
    _("  status [--show-credentials]                - Show current status\n"
      "\n"
      "Options:\n"
      "  --headless                                 - Use headless credentials storage\n"
      "  --system                                   - Configure system daemon\n"
      "  --help                                     - Print this help text\n");

  print_usage ();
  printf ("%s"
#ifdef HAVE_RDP
          "%s"
#endif
#ifdef HAVE_VNC
          "%s"
#endif
          "%s",
          help_header,
#ifdef HAVE_RDP
          help_rdp,
#endif
#ifdef HAVE_VNC
          help_vnc,
#endif
          help_other);
}

static GrdSettings *
create_settings (GrdRuntimeMode runtime_mode)
{
  switch (runtime_mode)
    {
    case GRD_RUNTIME_MODE_SCREEN_SHARE:
    case GRD_RUNTIME_MODE_HEADLESS:
      return GRD_SETTINGS (grd_settings_user_new (runtime_mode));
    case GRD_RUNTIME_MODE_SYSTEM:
      return GRD_SETTINGS (grd_settings_system_new ());
    case GRD_RUNTIME_MODE_HANDOVER:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static const char *
status_to_string (gboolean enabled,
                  gboolean use_colors)
{
  if (use_colors)
    {
      if (enabled)
        return "\x1b[1menabled\033[m";
      else
        return "\x1b[1mdisabled\033[m";
    }
  else
    {
      if (enabled)
        return "enabled";
      else
        return "disabled";
    }
}

#ifdef HAVE_RDP
static void
print_rdp_status (GrdSettings *settings,
                  gboolean     use_colors,
                  gboolean     show_credentials)
{
  g_autofree char *tls_fingerprint = NULL;
  g_autofree char *tls_cert = NULL;
  g_autofree char *tls_key = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;
  gboolean negotiate_port;
  gboolean view_only;
  gboolean enabled;
  int port;

  g_object_get (G_OBJECT (settings),
                "rdp-server-fingerprint", &tls_fingerprint,
                "rdp-server-cert-path", &tls_cert,
                "rdp-server-key-path", &tls_key,
                "rdp-negotiate-port", &negotiate_port,
                "rdp-view-only", &view_only,
                "rdp-enabled", &enabled,
                "rdp-port", &port,
                NULL);

  printf ("RDP:\n");
  printf ("\tStatus: %s\n", status_to_string (enabled, use_colors));
  printf ("\tPort: %d\n", port);
  printf ("\tTLS certificate: %s\n", tls_cert);
  printf ("\tTLS fingerprint: %s\n", tls_fingerprint);
  printf ("\tTLS key: %s\n", tls_key);
  if (!GRD_IS_SETTINGS_SYSTEM (settings))
    {
      printf ("\tView-only: %s\n", view_only ? "yes" : "no");
      printf ("\tNegotiate port: %s\n", negotiate_port ? "yes" : "no");
    }

  grd_settings_get_rdp_credentials (settings,
                                    &username, &password,
                                    &error);
  if (error)
    {
      fprintf (stderr, "Failed to read credentials: %s.\n", error->message);
      return;
    }

  if (show_credentials)
    {
      printf ("\tUsername: %s\n", username);
      printf ("\tPassword: %s\n", password);
    }
  else
    {
      printf ("\tUsername: %s\n",
              (username && strlen (username) > 1) ? "(hidden)" : "(empty)");
      printf ("\tPassword: %s\n",
              (password && strlen (password) > 1) ? "(hidden)" : "(empty)");
    }
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static void
print_vnc_status (GrdSettings *settings,
                  gboolean     use_colors,
                  gboolean     show_credentials)
{
  int port;
  gboolean enabled;
  gboolean view_only;
  gboolean negotiate_port;
  GrdVncAuthMethod auth_method;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;

  g_object_get (G_OBJECT (settings),
                "vnc-port", &port,
                "vnc-enabled", &enabled,
                "vnc-view-only", &view_only,
                "vnc-auth-method", &auth_method,
                "vnc-negotiate-port", &negotiate_port,
                NULL);

  password = grd_settings_get_vnc_password (settings, &error);
  if (error)
    {
      fprintf (stderr, "Failed to lookup VNC credentials: %s\n", error->message);
      return;
    }

  printf ("VNC:\n");
  printf ("\tStatus: %s\n", status_to_string (enabled, use_colors));

  printf ("\tPort: %d\n", port);
  if (auth_method == GRD_VNC_AUTH_METHOD_PROMPT)
    printf ("\tAuth method: prompt\n");
  else if (auth_method == GRD_VNC_AUTH_METHOD_PASSWORD)
    printf ("\tAuth method: password\n");
  printf ("\tView-only: %s\n", view_only ? "yes" : "no");
  printf ("\tNegotiate port: %s\n", negotiate_port ? "yes" : "no");
  if (show_credentials)
    {
      printf ("\tPassword: %s\n", password);
    }
  else
    {
      printf ("\tPassword: %s\n",
              (password && strlen (password) > 1) ? "(hidden)" : "(empty)");
    }
}
#endif /* HAVE_VNC */

static char *
unit_active_state_to_string (GrdSystemdUnitActiveState active_state,
                             gboolean                  use_colors)
{
  char *state = "";

  switch (active_state)
    {
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVE:
      state = "active";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_RELOADING:
      state = "reloading";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_INACTIVE:
      state = "inactive";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_FAILED:
      state = "failed";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_ACTIVATING:
      state = "activating";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_DEACTIVATING:
      state = "deactivating";
      break;
    case GRD_SYSTEMD_UNIT_ACTIVE_STATE_UNKNOWN:
      state = "unknown";
      break;
    }

  if (use_colors)
    return g_strdup_printf ("\x1b[1m%s\033[m", state);
  else
    return g_strdup (state);
}

static void
print_service_status (GrdSettings *settings,
                      gboolean    use_colors)
{
  GBusType bus_type;
  GrdSystemdUnitActiveState active_state;
  g_autofree char *active_state_str = NULL;
  g_autoptr (GDBusProxy) unit_proxy = NULL;

  if (GRD_IS_SETTINGS_SYSTEM (settings))
      bus_type = G_BUS_TYPE_SYSTEM;
  else
      bus_type = G_BUS_TYPE_SESSION;

  if (!grd_systemd_get_unit (bus_type,
                             GRD_SYSTEMD_SERVICE,
                             &unit_proxy,
                             NULL))
    return;

  if (!grd_systemd_unit_get_active_state (unit_proxy,
                                          &active_state,
                                          NULL))
    return;

  active_state_str = unit_active_state_to_string (active_state, use_colors);

  printf ("Overall:\n");
  printf ("\tUnit status: %s\n", active_state_str);
}

static int
print_status (GrdSettings  *settings,
              int           argc,
              char        **argv)
{
  gboolean use_colors;
  gboolean show_credentials = FALSE;

  if (argc > 1)
    {
      return EX_USAGE;
    }
  else if (argc == 1)
    {
      if (strcmp (argv[0], "--show-credentials") == 0)
        {
          show_credentials = TRUE;
        }
      else
        {
          return EX_USAGE;
        }
    }

  use_colors = isatty (fileno (stdout));

  print_service_status (settings, use_colors);

#ifdef HAVE_RDP
  print_rdp_status (settings, use_colors, show_credentials);
#endif /* HAVE_RDP */
#ifdef HAVE_VNC
  if (!GRD_IS_SETTINGS_SYSTEM (settings))
    print_vnc_status (settings, use_colors, show_credentials);
#endif /* HAVE_VNC */

  return EXIT_SUCCESS;
}

int
main (int   argc,
      char *argv[])
{
  g_autoptr (GrdSettings) settings = NULL;
  GrdRuntimeMode runtime_mode;
  int i;
  int arg_shift;
  int exit_code = EX_USAGE;
  struct passwd *pw;

  g_set_prgname (argv[0]);
  g_log_set_handler (NULL, G_LOG_LEVEL_WARNING, log_handler, NULL);

  if (argc < 2)
    goto done;

  if (argc == 2 && strcmp (argv[1], "--help") == 0)
    {
      print_help ();
      exit_code = EXIT_SUCCESS;
      goto done;
    }

  if (argc > 2 && strcmp (argv[1], "--headless") == 0)
    {
      runtime_mode = GRD_RUNTIME_MODE_HEADLESS;
      i = 2;
    }
  else if (argc > 1 && strcmp (argv[1], "--system") == 0)
    {
      runtime_mode = GRD_RUNTIME_MODE_SYSTEM;
      i = 2;
    }
  else
    {
      runtime_mode = GRD_RUNTIME_MODE_SCREEN_SHARE;
      i = 1;
    }

  arg_shift = i + 1;

  if (runtime_mode == GRD_RUNTIME_MODE_SYSTEM)
    {
      g_autoptr (GStrvBuilder) builder = NULL;
      g_auto (GStrv) new_argv = NULL;
      g_autoptr (GError) error = NULL;
      int wait_status;
      int j;

      builder = g_strv_builder_new ();

      g_strv_builder_add (builder, "pkexec");
      g_strv_builder_add (builder, "--user");
      g_strv_builder_add (builder, GRD_USERNAME);
      g_strv_builder_add (builder, argv[0]);

      for (j = 2; j < argc; j++)
        g_strv_builder_add (builder, argv[j]);

      new_argv = g_strv_builder_end (builder);

      g_spawn_sync (NULL,
                    new_argv,
                    NULL,
                    G_SPAWN_SEARCH_PATH
                    | G_SPAWN_CHILD_INHERITS_STDIN
                    | G_SPAWN_CHILD_INHERITS_STDOUT,
                    NULL,
                    NULL,
                    NULL,
                    NULL,
                    &wait_status,
                    &error);
      if (error)
        {
          fprintf (stderr, "Failed to start the configuration of the system daemon: %s\n",
                   error->message);

          if (WIFEXITED (wait_status))
            exit_code = WEXITSTATUS (wait_status);
          else
            exit_code = EX_SOFTWARE;
        }
      else
        {
          exit_code = EXIT_SUCCESS;
        }

      goto done;
    }

  pw = getpwnam (GRD_USERNAME);
  if (geteuid () == pw->pw_uid)
    runtime_mode = GRD_RUNTIME_MODE_SYSTEM;

  settings = create_settings (runtime_mode);
  if (!settings)
    {
      fprintf (stderr, "Failed to initialize settings.\n");
      exit_code = EXIT_FAILURE;
      goto done;
    }

#ifdef HAVE_RDP
  if (strcmp (argv[i], "rdp") == 0)
    {
      exit_code = process_rdp_options (settings,
                                       argc - arg_shift, argv + arg_shift);
      goto done;
    }
#endif
#ifdef HAVE_VNC
  if (strcmp (argv[i], "vnc") == 0)
    {
      exit_code = process_vnc_options (settings,
                                       argc - arg_shift, argv + arg_shift);
      goto done;
    }
#endif

  if (strcmp (argv[i], "status") == 0)
    {
      exit_code = print_status (settings,
                                argc - arg_shift, argv + arg_shift);
      goto done;
    }

done:
  if (exit_code == EX_USAGE)
    print_usage ();

  return exit_code;
}
