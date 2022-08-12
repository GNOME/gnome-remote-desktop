/*
 * Copyright (C) 2015 Red Hat Inc.
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
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#include "config.h"

#include <gio/gio.h>
#include <stdio.h>
#include <string.h>

#include "grd-private.h"

int
main (int argc, char **argv)
{
  g_autoptr(GApplication) app = NULL;
  gboolean terminate = FALSE;
  GOptionEntry entries[] = {
    { "terminate", 0, 0, G_OPTION_ARG_NONE, &terminate,
      "Terminate the daemon", NULL },
    { NULL }
  };
  GError *error = NULL;
  GOptionContext *context;

  context = g_option_context_new ("- control gnome-remote-desktop");
  g_option_context_add_main_entries (context, entries, NULL);
  if (!g_option_context_parse (context, &argc, &argv, &error))
    {
      g_printerr ("Invalid option: %s\n", error->message);
      g_error_free (error);
      return 1;
    }

  if (!terminate)
    {
      g_printerr ("%s", g_option_context_get_help (context, TRUE, NULL));
      return 1;
    }

  app = g_application_new (GRD_DAEMON_USER_APPLICATION_ID, 0);
  if (!g_application_register (app, NULL, NULL))
    {
      g_warning ("Failed to register with application\n");
      return 1;
    }
  if (!g_application_get_is_registered (app))
    {
      g_warning ("Not registered\n");
      return 1;
    }
  if (!g_application_get_is_remote (app))
    {
      g_warning ("Failed to connect to application\n");
      return 1;
    }

  if (terminate)
    g_action_group_activate_action (G_ACTION_GROUP (app),
                                    "terminate", NULL);
  else
    g_assert_not_reached ();

  return 0;
}
