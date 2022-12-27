/*
 * Copyright (C) 2015,2022 Red Hat Inc.
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

#include "grd-debug.h"

#include <glib.h>

static const GDebugKey grd_debug_keys[] = {
  { "vnc", GRD_DEBUG_VNC },
  { "tpm", GRD_DEBUG_TPM },
  { "vk-validation", GRD_DEBUG_VK_VALIDATION },
  { "vk-times", GRD_DEBUG_VK_TIMES },
  { "va-times", GRD_DEBUG_VA_TIMES },
};

static GrdDebugFlags debug_flags;

static gpointer
init_debug_flags (gpointer user_data)
{
  const char *debug_env;

  debug_env = g_getenv ("GNOME_REMOTE_DESKTOP_DEBUG");
  if (debug_env)
    {
      debug_flags = g_parse_debug_string (debug_env,
                                          grd_debug_keys,
                                          G_N_ELEMENTS (grd_debug_keys));
    }

  return NULL;
}

GrdDebugFlags
grd_get_debug_flags (void)
{
  static GOnce debug_flags_once = G_ONCE_INIT;

  g_once (&debug_flags_once, init_debug_flags, NULL);

  return debug_flags;
}
