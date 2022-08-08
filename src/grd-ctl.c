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

#include "grd-credentials-file.h"
#include "grd-credentials-libsecret.h"
#include "grd-credentials-tpm.h"

#define GRD_RDP_SETTINGS_SCHEMA "org.gnome.desktop.remote-desktop.rdp"
#define GRD_VNC_SETTINGS_SCHEMA "org.gnome.desktop.remote-desktop.vnc"

typedef enum _CredentialsType
{
  CREDENTIALS_TYPE_HEADLESS,
  CREDENTIALS_TYPE_SCREEN_SHARE,
} CredentialsType;

typedef struct _SubCommand
{
  const char *subcommand;
  gboolean (* process) (CredentialsType   credentials_type,
                        int               argc,
                        char            **argv,
                        GError          **error);
  int n_args;
} SubCommand;

static void
print_usage (void)
{
  printf (_("Usage: %s [OPTIONS...] COMMAND [SUBCOMMAND]...\n"),
          g_get_prgname ());
}

static int
process_options (CredentialsType    credentials_type,
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

      if (!subcommands[i].process (credentials_type,
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

static GrdCredentials *
create_credentials (CredentialsType   credentials_type,
                    GError          **error)
{
  g_autoptr (GError) local_error = NULL;

  switch (credentials_type)
    {
    case CREDENTIALS_TYPE_SCREEN_SHARE:
      return GRD_CREDENTIALS (grd_credentials_libsecret_new ());
    case CREDENTIALS_TYPE_HEADLESS:
      {
        GrdCredentialsTpm *credentials_tpm;
        GrdCredentialsFile *credentials_file;

        credentials_tpm = grd_credentials_tpm_new (&local_error);
        if (credentials_tpm)
          return GRD_CREDENTIALS (credentials_tpm);

        g_warning ("Init TPM credentials failed because %s, using GKeyFile as fallback",
                   local_error->message);
        credentials_file = grd_credentials_file_new (error);
        if (credentials_file)
          return GRD_CREDENTIALS (credentials_file);

        return NULL;
      }
    }

  g_assert_not_reached ();
}

#ifdef HAVE_RDP
static gboolean
grd_store_rdp_credentials (CredentialsType   credentials_type,
                           const char       *username,
                           const char       *password,
                           GError          **error)
{
  g_autoptr (GrdCredentials) credentials = NULL;
  GVariantBuilder builder;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));
  g_variant_builder_add (&builder, "{sv}",
                         "username", g_variant_new_string (username));
  g_variant_builder_add (&builder, "{sv}",
                         "password", g_variant_new_string (password));

  credentials = create_credentials (credentials_type, error);
  if (!credentials)
    return FALSE;

  return grd_credentials_store (credentials,
                                GRD_CREDENTIALS_TYPE_RDP,
                                g_variant_builder_end (&builder),
                                error);
}

static gboolean
grd_clear_rdp_credentials (CredentialsType   credentials_type,
                           GError          **error)
{
  g_autoptr (GrdCredentials) credentials = NULL;

  credentials = create_credentials (credentials_type, error);
  if (!credentials)
    return FALSE;

  return grd_credentials_clear (credentials, GRD_CREDENTIALS_TYPE_RDP, error);
}

static gboolean
rdp_enable (CredentialsType   credentials_type,
            int               argc,
            char            **argv,
            GError          **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "enable", TRUE);
  return TRUE;
}

static gboolean
rdp_disable (CredentialsType   credentials_type,
             int               argc,
             char            **argv,
             GError          **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "enable", FALSE);
  return TRUE;
}

static gboolean
rdp_set_tls_cert (CredentialsType   credentials_type,
                  int               argc,
                  char            **argv,
                  GError          **error)
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
rdp_set_tls_key (CredentialsType   credentials_type,
                 int               argc,
                 char            **argv,
                 GError          **error)
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
rdp_set_credentials (CredentialsType   credentials_type,
                     int               argc,
                     char            **argv,
                     GError          **error)
{
  return grd_store_rdp_credentials (credentials_type,
                                    argv[0], argv[1], error);
}

static gboolean
rdp_clear_credentials (CredentialsType   credentials_type,
                       int               argc,
                       char            **argv,
                       GError          **error)
{
  return grd_clear_rdp_credentials (credentials_type, error);
}

static gboolean
rdp_enable_view_only (CredentialsType   credentials_type,
                      int               argc,
                      char            **argv,
                      GError          **error)
{
  g_autoptr (GSettings) rdp_settings = NULL;

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);
  g_settings_set_boolean (rdp_settings, "view-only", TRUE);
  return TRUE;
}

static gboolean
rdp_disable_view_only (CredentialsType   credentials_type,
                       int               argc,
                       char            **argv,
                       GError          **error)
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
process_rdp_options (CredentialsType   credentials_type,
                     int               argc,
                     char            **argv)
{
  return process_options (credentials_type,
                          argc, argv,
                          rdp_subcommands,
                          G_N_ELEMENTS (rdp_subcommands));
}
#endif /* HAVE_RDP */

#ifdef HAVE_VNC
static gboolean
vnc_enable (CredentialsType   credentials_type,
            int               argc,
            char            **argv,
            GError          **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "enable", TRUE);
  return TRUE;
}

static gboolean
vnc_disable (CredentialsType   credentials_type,
             int               argc,
             char            **argv,
             GError          **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "enable", FALSE);
  return TRUE;
}

#define MAX_VNC_PASSWORD_SIZE 8

static gboolean
vnc_set_credentials (CredentialsType   credentials_type,
                     int               argc,
                     char            **argv,
                     GError          **error)
{
  g_autoptr (GrdCredentials) credentials = NULL;
  char *password;

  password = argv[0];
  if (strlen (password) > MAX_VNC_PASSWORD_SIZE)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                   "Password is too long");
      return FALSE;
    }

  credentials = create_credentials (credentials_type, error);
  if (!credentials)
    return FALSE;

  return grd_credentials_store (credentials,
                                GRD_CREDENTIALS_TYPE_VNC,
                                g_variant_new_string (password),
                                error);
}

static gboolean
vnc_clear_credentials (CredentialsType   credentials_type,
                       int               argc,
                       char            **argv,
                       GError          **error)
{
  g_autoptr (GrdCredentials) credentials = NULL;

  credentials = create_credentials (credentials_type, error);
  if (!credentials)
    return FALSE;

  return grd_credentials_clear (credentials, GRD_CREDENTIALS_TYPE_VNC, error);
}

static gboolean
vnc_set_auth_method (CredentialsType   credentials,
                     int               argc,
                     char            **argv,
                     GError          **error)
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
vnc_enable_view_only (CredentialsType   credentials,
                      int               argc,
                      char            **argv,
                      GError          **error)
{
  g_autoptr (GSettings) vnc_settings = NULL;

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);
  g_settings_set_boolean (vnc_settings, "view-only", TRUE);
  return TRUE;
}

static gboolean
vnc_disable_view_only (CredentialsType   credentials,
                       int               argc,
                       char            **argv,
                       GError          **error)
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
process_vnc_options (CredentialsType   credentials_type,
                     int               argc,
                     char            **argv)
{
  return process_options (credentials_type,
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
print_rdp_status (CredentialsType credentials_type,
                  gboolean        use_colors,
                  gboolean        show_credentials)
{
  g_autoptr (GrdCredentials) credentials = NULL;
  g_autoptr (GSettings) rdp_settings = NULL;
  g_autofree char *tls_cert = NULL;
  g_autofree char *tls_key = NULL;
  g_autoptr (GVariant) rdp_credentials = NULL;
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;
  g_autoptr (GError) error = NULL;

  credentials = create_credentials (credentials_type, &error);
  if (!credentials)
    {
      fprintf (stderr, "Failed to initialize credential manager: %s\n",
               error->message);
      return;
    }

  rdp_credentials = grd_credentials_lookup (credentials,
                                            GRD_CREDENTIALS_TYPE_RDP,
                                            &error);
  if (error)
    {
      fprintf (stderr, "Failed to lookup RDP credentials: %s\n", error->message);
      return;
    }

  rdp_settings = g_settings_new (GRD_RDP_SETTINGS_SCHEMA);

  printf ("RDP:\n");
  printf ("\tStatus: %s\n",
          status_to_string (g_settings_get_boolean (rdp_settings, "enable"),
                            use_colors));

  tls_cert = g_settings_get_string (rdp_settings, "tls-cert");
  printf ("\tTLS certificate: %s\n", tls_cert);
  tls_key = g_settings_get_string (rdp_settings, "tls-key");
  printf ("\tTLS key: %s\n", tls_key);
  printf ("\tView-only: %s\n",
          g_settings_get_boolean (rdp_settings, "view-only") ? "yes" : "no");

  if (rdp_credentials)
    {
      g_variant_lookup (rdp_credentials, "username", "s", &username);
      g_variant_lookup (rdp_credentials, "password", "s", &password);
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
print_vnc_status (CredentialsType credentials_type,
                  gboolean        use_colors,
                  gboolean        show_credentials)
{
  g_autoptr (GrdCredentials) credentials = NULL;
  g_autoptr (GSettings) vnc_settings = NULL;
  g_autoptr (GVariant) password_variant = NULL;
  g_autofree char *auth_method = NULL;
  const char *password = NULL;
  g_autoptr (GError) error = NULL;

  credentials = create_credentials (credentials_type, &error);
  if (!credentials)
    {
      fprintf (stderr, "Failed to initialize credential manager: %s\n",
               error->message);
      return;
    }

  password_variant = grd_credentials_lookup (credentials,
                                             GRD_CREDENTIALS_TYPE_VNC,
                                             &error);
  if (error)
    {
      fprintf (stderr, "Failed to lookup VNC credentials: %s\n", error->message);
      return;
    }

  if (password_variant)
    password = g_variant_get_string (password_variant, NULL);

  vnc_settings = g_settings_new (GRD_VNC_SETTINGS_SCHEMA);

  printf ("VNC:\n");
  printf ("\tStatus: %s\n",
          status_to_string (g_settings_get_boolean (vnc_settings, "enable"),
                            use_colors));

  auth_method = g_settings_get_string (vnc_settings, "auth-method");
  printf ("\tAuth method: %s\n", auth_method);
  printf ("\tView-only: %s\n",
          g_settings_get_boolean (vnc_settings, "view-only") ? "yes" : "no");
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
print_status (CredentialsType   credentials_type,
              int               argc,
              char            **argv)
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
  print_rdp_status (credentials_type, use_colors, show_credentials);
#endif /* HAVE_RDP */
#ifdef HAVE_VNC
  print_vnc_status (credentials_type, use_colors, show_credentials);
#endif /* HAVE_VNC */

  return EXIT_SUCCESS;
}

int
main (int   argc,
      char *argv[])
{
  CredentialsType credentials_type;
  int i;
  int arg_shift;

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

  if (argc > 2 && strcmp (argv[1], "--headless") == 0)
    {
      credentials_type = CREDENTIALS_TYPE_HEADLESS;
      i = 2;
    }
  else
    {
      credentials_type = CREDENTIALS_TYPE_SCREEN_SHARE;
      i = 1;
    }

  arg_shift = i + 1;

#ifdef HAVE_RDP
  if (strcmp (argv[i], "rdp") == 0)
    {
      return process_rdp_options (credentials_type,
                                  argc - arg_shift, argv + arg_shift);
    }
#endif
#ifdef HAVE_VNC
  if (strcmp (argv[i], "vnc") == 0)
    {
      return process_vnc_options (credentials_type,
                                  argc - arg_shift, argv + arg_shift);
    }
#endif

  if (strcmp (argv[i], "status") == 0)
    {
      return print_status (credentials_type,
                           argc - arg_shift, argv + arg_shift);
    }

  print_usage ();
  return EXIT_FAILURE;
}
