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

#include <gio/gio.h>
#include <glib/gi18n.h>
#include <stdio.h>

#include "grd-enums.h"
#include "grd-settings-user.h"

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
  int i;

  if (argc <= 0)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  for (i = 0; i < n_subcommands; i++)
    {
      g_autoptr (GError) error = NULL;

      if (g_strcmp0 (argv[0], subcommands[i].subcommand) != 0)
        continue;

      if (subcommands[i].n_args != argc - 1)
        {
          g_printerr ("Wrong number of arguments for subcommand '%s'\n",
                      argv[0]);
          return EXIT_FAILURE;
        }

      if (!subcommands[i].process (settings,
                                   argc - 1, argv + 1, &error))
        {
          g_printerr ("%s\n", error->message);
          return EXIT_FAILURE;
        }

      g_settings_sync ();
      return EXIT_SUCCESS;
    }

  g_printerr ("Unknown subcommand '%s'\n", argv[0]);
  return EXIT_FAILURE;
}

#ifdef HAVE_RDP
static gboolean
rdp_enable (GrdSettings  *settings,
            int           argc,
            char        **argv,
            GError      **error)
{
  g_object_set (G_OBJECT (settings), "rdp-enabled", TRUE, NULL);
  return TRUE;
}

static gboolean
rdp_disable (GrdSettings  *settings,
             int           argc,
             char        **argv,
             GError      **error)
{
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

  g_object_set (G_OBJECT (settings), "rdp-server-cert", argv[0], NULL);
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

  g_object_set (G_OBJECT (settings), "rdp-server-key", argv[0], NULL);
  return TRUE;
}

static gboolean
rdp_set_credentials (GrdSettings  *settings,
                     int           argc,
                     char        **argv,
                     GError      **error)
{
  return grd_settings_set_rdp_credentials (settings, argv[0], argv[1], error);
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

static const SubCommand rdp_subcommands[] = {
  { "enable", rdp_enable, 0 },
  { "disable", rdp_disable, 0 },
  { "set-tls-cert", rdp_set_tls_cert, 1 },
  { "set-tls-key", rdp_set_tls_key, 1 },
  { "set-credentials", rdp_set_credentials, 2 },
  { "clear-credentials", rdp_clear_credentials, 0 },
  { "enable-view-only", rdp_enable_view_only, 0 },
  { "disable-view-only", rdp_disable_view_only, 0 },
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
  char *password;

  password = argv[0];
  if (strlen (password) > MAX_VNC_PASSWORD_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is too long");
      return FALSE;
    }

  return grd_settings_set_vnc_password (settings, argv[0], error);
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

static const SubCommand vnc_subcommands[] = {
  { "enable", vnc_enable, 0 },
  { "disable", vnc_disable, 0 },
  { "set-password", vnc_set_credentials, 1 },
  { "clear-password", vnc_clear_credentials, 0 },
  { "set-auth-method", vnc_set_auth_method, 1 },
  { "enable-view-only", vnc_enable_view_only, 0 },
  { "disable-view-only", vnc_disable_view_only, 0 },
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
    _("  rdp                                        - RDP subcommands:\n"
      "    enable                                   - Enable the RDP backend\n"
      "    disable                                  - Disable the RDP backend\n"
      "    set-tls-cert <path-to-cert>              - Set path to TLS certificate\n"
      "    set-tls-key <path-to-key>                - Set path to TLS key\n"
      "    set-credentials <username> <password>    - Set username and password\n"
      "                                               credentials\n"
      "    clear-credentials                        - Clear username and password\n"
      "                                               credentials\n"
      "    enable-view-only                         - Disable remote control of input\n"
      "                                               devices\n"
      "    disable-view-only                        - Enable remote control of input\n"
      "                                               devices\n"
      "\n");
#endif /* HAVE_RDP */
#ifdef HAVE_VNC
  /* For translators: This first words on each line is the command;
   * don't translate. Try to fit each line within 80 characters. */
  const char *help_vnc =
    _("  vnc                                        - VNC subcommands:\n"
      "    enable                                   - Enable the VNC backend\n"
      "    disable                                  - Disable the VNC backend\n"
      "    set-password <password>                  - Set the VNC password\n"
      "    clear-password                           - Clear the VNC password\n"
      "    set-auth-method password|prompt          - Set the authorization method\n"
      "    enable-view-only                         - Disable remote control of input\n"
      "                                               devices\n"
      "    disable-view-only                        - Enable remote control of input\n"
      "                                               devices\n"
      "\n");
#endif /* HAVE_VNC */
  /* For translators: This first words on each line is the command;
   * don't translate. Try to fit each line within 80 characters. */
  const char *help_other =
    _("  status [--show-credentials]                - Show current status\n"
      "\n"
      "Options:\n"
      "  --headless                                 - Use headless credentials storage\n"
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
  gboolean enabled;
  gboolean view_only;
  g_autofree char *tls_cert = NULL;
  g_autofree char *tls_key = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;

  g_object_get (G_OBJECT (settings),
                "rdp-enabled", &enabled,
                "rdp-server-key", &tls_key,
                "rdp-server-cert", &tls_cert,
                "rdp-view-only", &view_only,
                NULL);

  printf ("RDP:\n");
  printf ("\tStatus: %s\n", status_to_string (enabled, use_colors));

  printf ("\tTLS certificate: %s\n", tls_cert);
  printf ("\tTLS key: %s\n", tls_key);
  printf ("\tView-only: %s\n", view_only ? "yes" : "no");

  grd_settings_get_rdp_credentials (settings,
                                    &username, &password,
                                    &error);
  if (error)
    {
      fprintf (stderr, "Failed reading credentials: %s.\n", error->message);
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
  gboolean enabled;
  gboolean view_only;
  GrdVncAuthMethod auth_method;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;

  g_object_get (G_OBJECT (settings),
                "vnc-enabled", &enabled,
                "vnc-view-only", &view_only,
                "vnc-auth-method", &auth_method,
                NULL);

  password = grd_settings_get_vnc_password (settings, &error);
  if (error)
    {
      fprintf (stderr, "Failed to lookup VNC credentials: %s\n", error->message);
      return;
    }

  printf ("VNC:\n");
  printf ("\tStatus: %s\n", status_to_string (enabled, use_colors));

  if (auth_method == GRD_VNC_AUTH_METHOD_PROMPT)
    printf ("\tAuth method: prompt\n");
  else if (auth_method == GRD_VNC_AUTH_METHOD_PASSWORD)
    printf ("\tAuth method: password\n");
  printf ("\tView-only: %s\n", view_only ? "yes" : "no");
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

static int
print_status (GrdSettings  *settings,
              int           argc,
              char        **argv)
{
  gboolean use_colors;
  gboolean show_credentials = FALSE;

  if (argc > 1)
    {
      print_usage ();
      return EXIT_FAILURE;
    }
  else if (argc == 1)
    {
      if (strcmp (argv[0], "--show-credentials") == 0)
        {
          show_credentials = TRUE;
        }
      else
        {
          print_usage ();
          return EXIT_FAILURE;
        }
    }

  use_colors = isatty (fileno (stdout));

#ifdef HAVE_RDP
  print_rdp_status (settings, use_colors, show_credentials);
#endif /* HAVE_RDP */
#ifdef HAVE_VNC
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

  g_set_prgname (argv[0]);
  g_log_set_handler (NULL, G_LOG_LEVEL_WARNING, log_handler, NULL);

  if (argc < 2)
    {
      print_usage ();
      return EXIT_FAILURE;
    }

  if (argc == 2 && strcmp (argv[1], "--help") == 0)
    {
      print_help ();
      return EXIT_SUCCESS;
    }

  if (argc > 2 && strcmp (argv[1], "--headless") == 0)
    {
      runtime_mode = GRD_RUNTIME_MODE_HEADLESS;
      i = 2;
    }
  else
    {
      runtime_mode = GRD_RUNTIME_MODE_SCREEN_SHARE;
      i = 1;
    }

  arg_shift = i + 1;

  settings = create_settings (runtime_mode);
  if (!settings)
    {
      fprintf (stderr, "Failed to initialize settings.\n");
      return EXIT_FAILURE;
    }

#ifdef HAVE_RDP
  if (strcmp (argv[i], "rdp") == 0)
    {
      return process_rdp_options (settings,
                                  argc - arg_shift, argv + arg_shift);
    }
#endif
#ifdef HAVE_VNC
  if (strcmp (argv[i], "vnc") == 0)
    {
      return process_vnc_options (settings,
                                  argc - arg_shift, argv + arg_shift);
    }
#endif

  if (strcmp (argv[i], "status") == 0)
    {
      return print_status (settings,
                           argc - arg_shift, argv + arg_shift);
    }

  print_usage ();
  return EXIT_FAILURE;
}
