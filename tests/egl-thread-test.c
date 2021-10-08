/*
 * Copyright (C) 2021 Red Hat Inc.
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

#include "grd-egl-thread.h"

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  GrdEglThread *egl_thread;

  egl_thread = grd_egl_thread_new (&error);
  if (!egl_thread)
    g_error ("Failed to start EGL thread: %s", error->message);

  grd_egl_thread_free (egl_thread);

  return EXIT_SUCCESS;
}
