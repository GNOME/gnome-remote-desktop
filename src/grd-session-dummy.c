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

#include "grd-session-dummy.h"

#include <pinos/client/stream.h>
#include <glib.h>
#include <gst/gst.h>

#include "grd-stream.h"

struct _GrdSessionDummy
{
  GrdSession parent;

  GstElement *pipeline;
};

G_DEFINE_TYPE (GrdSessionDummy, grd_session_dummy, GRD_TYPE_SESSION);

static void
grd_session_dummy_stream_added (GrdSession *session,
                                GrdStream  *stream)
{
  GrdSessionDummy *session_dummy = GRD_SESSION_DUMMY (session);
  const char *source_path;
  char *pipeline_str;
  GError *error = NULL;

  source_path = grd_stream_get_pinos_source_path (stream);
  pipeline_str =
    g_strdup_printf ("pinossrc name=pinossrc path=%s ! videoconvert ! ximagesink",
                     source_path);

  session_dummy->pipeline = gst_parse_launch (pipeline_str, &error);
  if (!session_dummy->pipeline)
    {
      g_warning ("Failed to start dummy pipeline: %s\n", error->message);
    }

  gst_element_set_state (session_dummy->pipeline, GST_STATE_PLAYING);

  g_free (pipeline_str);
}

static void
grd_session_dummy_stop (GrdSession *session)
{
  GrdSessionDummy *session_dummy = GRD_SESSION_DUMMY (session);
  GstElement *pinossrc;
  PinosStream *pinos_stream;

  pinossrc = gst_bin_get_by_name (GST_BIN (session_dummy->pipeline),
                                  "pinossrc");
  g_object_get (pinossrc, "stream", &pinos_stream, NULL);
  pinos_stream_disconnect (pinos_stream);
  gst_element_set_state (session_dummy->pipeline, GST_STATE_NULL);
  g_clear_pointer (&session_dummy->pipeline, gst_object_unref);
}

static void
grd_session_dummy_init (GrdSessionDummy *session_dummy)
{
}

static void
grd_session_dummy_class_init (GrdSessionDummyClass *klass)
{
  GrdSessionClass *session_class = GRD_SESSION_CLASS (klass);

  session_class->stream_added = grd_session_dummy_stream_added;
  session_class->stop = grd_session_dummy_stop;
}
