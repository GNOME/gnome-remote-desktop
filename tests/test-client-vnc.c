/*
 * Copyright (C) 2019 Red Hat Inc.
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

#include <glib.h>
#include <rfb/rfbclient.h>
#include <stdio.h>

static gboolean saw_correct_size = FALSE;

static rfbBool
handle_malloc_framebuffer (rfbClient *rfb_client)
{
  if (rfb_client->width == 1024 &&
      rfb_client->height == 768)
    saw_correct_size = TRUE;

  g_clear_pointer (&rfb_client->frameBuffer, g_free);
  rfb_client->frameBuffer = g_malloc0 (rfb_client->width *
                                       rfb_client->height *
                                       rfb_client->format.bitsPerPixel / 4);

  return TRUE;
}

static void
handle_got_framebuffer_update (rfbClient *rfb_client,
                               int        x,
                               int        y,
                               int        width,
                               int        height)
{
  if (saw_correct_size)
    exit(EXIT_SUCCESS);
}

static char *
handle_get_password (rfbClient *rfb_client)
{
  const char *test_password;

  test_password = g_getenv ("GNOME_REMOTE_DESKTOP_TEST_VNC_PASSWORD");
  g_assert (test_password);

  return g_strdup (test_password);
}

int
main (int    argc,
      char **argv)
{
  rfbClient *rfb_client;
  const int bits_per_sample = 8;
  const int samples_per_pixel = 3;
  const int bytes_per_pixel = 4;

  rfb_client = rfbGetClient (bits_per_sample,
                             samples_per_pixel,
                             bytes_per_pixel);

  rfb_client->MallocFrameBuffer = handle_malloc_framebuffer;
  rfb_client->canHandleNewFBSize = TRUE;
  rfb_client->GotFrameBufferUpdate = handle_got_framebuffer_update;
  rfb_client->GetPassword = handle_get_password;
  rfb_client->listenPort = LISTEN_PORT_OFFSET;
  rfb_client->listen6Port = LISTEN_PORT_OFFSET;

  if (!rfbInitClient (rfb_client, &argc, argv))
    {
      g_warning ("Failed to initialize VNC client");
      return EXIT_FAILURE;
    }

  while (TRUE)
    {
      int ret;
      const int timeout_us = 500;

      ret = WaitForMessage (rfb_client, timeout_us);
      if (ret < 0)
        {
          g_warning ("WaitForMessage failed");
          rfbClientCleanup (rfb_client);
          return EXIT_FAILURE;
        }
      else if (ret > 0)
        {
          if (!HandleRFBServerMessage (rfb_client))
            {
              g_warning ("HandleRFBServerMessage failed");
              rfbClientCleanup (rfb_client);
              return EXIT_FAILURE;
            }
        }
    }
}
