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

#include <glib/gi18n.h>
#include <libsecret/secret.h>
#include <stdio.h>

#include "grd-schemas.h"

#define GRD_RDP_SETTINGS_SCHEMA "org.gnome.desktop.remote-desktop.rdp"
#define GRD_VNC_SETTINGS_SCHEMA "org.gnome.desktop.remote-desktop.vnc"

typedef struct _SubCommand
{
  const char *subcommand;
  gboolean (* process) (int      argc,
                        char   **argv,
                        GError **error);
  int n_args;
} SubCommand;

static void
print_usage (void)
{
  printf (_("Usage: %s [OPTIONS...] COMMAND [SUBCOMMAND]...\n"),
          g_get_prgname ());
}

static int
process_options (int                argc,
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

      if (!subcommands[i].process (argc - 1, argv + 1, &error))
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
grd_store_rdp_credentials (const char  *username,
                           const char  *password,
                           GError     **error)
{
  GVariantBuilder builder;
  g_autofree char *credentials = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}",
                         "username", g_variant_new_string (username));
  g_variant_builder_add (&builder, "{sv}",
                         "password", g_variant_new_string (password));
  credentials = g_variant_print (g_variant_builder_end (&builder), TRUE);

  if (!secret_password_store_sync (GRD_RDP_CREDENTIALS_SCHEMA,
                                   SECRET_COLLECTION_DEFAULT,
                                   "GNOME Remote Desktop RDP credentials",
                                   credentials,
                                   NULL, error,
                                   NULL))
    return FALSE;

  return TRUE;
}

static gboolean
grd_clear_rdp_credentials (GError **error)
{
  return secret_password_clear_sync (GRD_RDP_CREDENTIALS_SCHEMA,
                                     NULL, error,
                                     NULL);
}

static gboolean
rdp_enable (int      argc,
            char   **argv,
            GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "enable", TRUE);
  return TRUE;
}

static gboolean
rdp_disable (int      argc,
             char   **argv,
             GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "enable", FALSE);
  return TRUE;
}

static gboolean
rdp_set_tls_cert (int      argc,
                  char   **argv,
                  GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;
  char *path = argv[0];

  if (!g_path_is_absolute (path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Path to certificate file not absolute");
      return FALSE;
    }

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_string (rdp_settings, "tls-cert", argv[0]);
  return TRUE;
}

static gboolean
rdp_set_tls_key (int      argc,
                 char   **argv,
                 GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;
  char *path = argv[0];

  if (!g_path_is_absolute (path))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                   "Path to certificate file not absolute");
      return FALSE;
    }

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_string (rdp_settings, "tls-key", argv[0]);
  return TRUE;
}

static gboolean
rdp_set_credentials (int      argc,
                     char   **argv,
                     GError **error)
{
  return grd_store_rdp_credentials (argv[0], argv[1], error);
}

static gboolean
rdp_clear_credentials (int      argc,
                       char   **argv,
                       GError **error)
{
  return grd_clear_rdp_credentials (error);
}

static gboolean
rdp_enable_view_only (int      argc,
                      char   **argv,
                      GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "view-only", TRUE);
  return TRUE;
}

static gboolean
rdp_disable_view_only (int      argc,
                       char   **argv,
                       GError **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "view-only", FALSE);
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
process_rdp_options (int    argc,
                     char **argv)
{
  return process_options (argc, argv,
                          rdp_subcommands,
                          G_N_ELEMENTS (rdp_subcommands));
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static gboolean
vnc_enable (int      argc,
            char   **argv,
            GError **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "enable", TRUE);
  return TRUE;
}

static gboolean
vnc_disable (int      argc,
             char   **argv,
             GError **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "enable", FALSE);
  return TRUE;
}

#define MAX_VNC_PASSWORD_SIZE 8

static gboolean
vnc_set_credentials (int      argc,
                     char   **argv,
                     GError **error)
{
  char *password;

  password = argv[0];
  if (strlen (password) > MAX_VNC_PASSWORD_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is too long");
      return FALSE;
    }

  if (!secret_password_store_sync (GRD_VNC_PASSWORD_SCHEMA,
                                   SECRET_COLLECTION_DEFAULT,
                                   "GNOME Remote Desktop VNC password",
                                   password,
                                   NULL, error,
                                   NULL))
    return FALSE;

  return TRUE;
}

static gboolean
vnc_clear_credentials (int      argc,
                       char   **argv,
                       GError **error)
{
  return secret_password_clear_sync (GRD_VNC_PASSWORD_SCHEMA,
                                     NULL, error,
                                     NULL);
}

static gboolean
vnc_set_auth_method (int      argc,
                     char   **argv,
                     GError **error)
{
  char *auth_method = argv[0];
  g_autoptr (GSettings) vnc_settings = NULL;

  if (strcmp (auth_method, "prompt") != 0 &&
      strcmp (auth_method, "password") != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Invalid auth method '%s'", auth_method);
      return FALSE;
    }

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_string (vnc_settings, "auth-method", auth_method);
  return TRUE;
}

static gboolean
vnc_enable_view_only (int      argc,
                      char   **argv,
                      GError **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "view-only", TRUE);
  return TRUE;
}

static gboolean
vnc_disable_view_only (int      argc,
                       char   **argv,
                       GError **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "view-only", FALSE);
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
process_vnc_options (int    argc,
                     char **argv)
{
  return process_options (argc, argv,
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
    _("Options:\n"
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

int
main (int   argc,
      char *argv[])
{
  g_set_prgname (argv[0]);

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

#ifdef HAVE_RDP
  if (strcmp (argv[1], "rdp") == 0)
    {
      return process_rdp_options (argc - 2, argv + 2);
    }
#endif
#ifdef HAVE_VNC
  if (strcmp (argv[1], "vnc") == 0)
    {
      return process_vnc_options (argc - 2, argv + 2);
    }
#endif

  print_usage ();
  return EXIT_FAILURE;
}
