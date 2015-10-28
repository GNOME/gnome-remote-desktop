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

  if (argc != 2 || strcmp (argv[1], "--toggle-record") != 0)
    {
      fprintf (stderr, "Usage %s --toggle-record\n", argv[0]);
      return -1;
    }

  app = g_application_new (GRD_DAEMON_APPLICATION_ID, 0);
  if (!g_application_register (app, NULL, NULL))
    {
      g_error ("Failed to register with application\n");
    }
  if (!g_application_get_is_registered (app))
    {
      g_error ("Not registered\n");
    }
  if (!g_application_get_is_remote (app))
    {
      g_error ("Failed to connect to application\n");
    }
  g_action_group_activate_action (G_ACTION_GROUP (app), "toggle-record", NULL);

  return 0;
}
