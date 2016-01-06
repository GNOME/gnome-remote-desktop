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

#include "grd-vnc-sink.h"

#include <gst/video/gstvideometa.h>
#include <gst/video/video-format.h>

#include "grd-context.h"
#include "grd-session-vnc.h"

struct _GrdVncSink
{
  GstBaseSink parent;

  GstCaps *caps;
  GMutex lock;

  int video_width;
  int video_height;
  GstBuffer *pending_buffer;

  GrdSessionVnc *session_vnc;
};

G_DEFINE_TYPE (GrdVncSink, grd_vnc_sink, GST_TYPE_BASE_SINK);

static GstCaps *
grd_vnc_sink_get_caps (GstBaseSink *base_sink,
                       GstCaps     *filter)
{
  GrdVncSink *vnc_sink = GRD_VNC_SINK (base_sink);

  if (filter)
    {
      return gst_caps_intersect_full (filter,
                                      vnc_sink->caps,
                                      GST_CAPS_INTERSECT_FIRST);
    }
  else
    {
      return gst_caps_ref (vnc_sink->caps);
    }
}

static gboolean
grd_vnc_sink_set_caps (GstBaseSink *base_sink,
                       GstCaps     *caps)
{
  GrdVncSink *vnc_sink = GRD_VNC_SINK (base_sink);
  GstVideoInfo info;

  if (!gst_caps_can_intersect (vnc_sink->caps, caps))
    {
      GST_ERROR_OBJECT (vnc_sink, "caps incompatible");
      return FALSE;
    }

  if (!gst_video_info_from_caps (&info, caps))
    {
      GST_ERROR_OBJECT (vnc_sink, "caps invalid");
    }

  g_mutex_lock (&vnc_sink->lock);
  vnc_sink->video_width = info.width;
  vnc_sink->video_height = info.height;
  g_mutex_unlock (&vnc_sink->lock);

  return TRUE;
}

static gboolean
grd_vnc_sink_render_on_main_thread (GrdVncSink *vnc_sink)
{
  GrdSessionVnc *session_vnc = vnc_sink->session_vnc;
  GstBuffer *buffer;
  int width, height;

  g_mutex_lock (&vnc_sink->lock);

  /* Check if session disappeared while callback was queued. */
  if (!session_vnc)
    {
      g_mutex_unlock (&vnc_sink->lock);
      gst_object_unref (vnc_sink);
      return FALSE;
    }
  gst_object_unref (vnc_sink);

  buffer = vnc_sink->pending_buffer;
  vnc_sink->pending_buffer = NULL;
  width = vnc_sink->video_width;
  height = vnc_sink->video_height;
  g_mutex_unlock (&vnc_sink->lock);

  if (!buffer)
    return FALSE;

  grd_session_vnc_resize_framebuffer (session_vnc, width, height);
  grd_session_vnc_draw_buffer (session_vnc, buffer);
  gst_buffer_unref (buffer);

  return FALSE;
}

static GstFlowReturn
grd_vnc_sink_render (GstBaseSink *base_sink,
                     GstBuffer   *buffer)
{
  GrdVncSink *vnc_sink = GRD_VNC_SINK (base_sink);
  GrdSession *session;
  GrdContext *context;

  if (!vnc_sink->session_vnc)
    return GST_FLOW_OK;

  session = GRD_SESSION (vnc_sink->session_vnc);
  context = grd_session_get_context (session);

  g_mutex_lock (&vnc_sink->lock);
  g_clear_pointer (&vnc_sink->pending_buffer, gst_buffer_unref);
  vnc_sink->pending_buffer = gst_buffer_ref (buffer);
  g_mutex_unlock (&vnc_sink->lock);

  g_main_context_invoke (grd_context_get_main_context (context),
                         (GSourceFunc) grd_vnc_sink_render_on_main_thread,
                         gst_object_ref (vnc_sink));

  return GST_FLOW_OK;
}

static gboolean
plugin_init (GstPlugin *plugin)
{
  gst_element_register (plugin, "grdvncsink", GST_RANK_NONE, GRD_TYPE_VNC_SINK);
  return TRUE;
}

static void
grd_vnc_sink_register (void)
{
  static gboolean registered = FALSE;

  if (registered)
    return;

  gst_plugin_register_static (GST_VERSION_MAJOR, GST_VERSION_MINOR,
                              "grdvnc",
                              "GNOME Remote Desktop VNC sink plugin",
                              plugin_init,
                              "0.1",
                              "LGPL",
                              "gnome-remote-desktop",
                              "gnome-remote-desktop",
                              "http://www.gnome.org/");
  registered = TRUE;
}

static void
on_session_stopped (GrdSessionVnc *session_vnc,
                    GrdVncSink    *vnc_sink)
{
  vnc_sink->session_vnc = NULL;
}

GstElement *
grd_vnc_sink_new (GrdSessionVnc *session_vnc)
{
  GstElement *element;
  GrdVncSink *vnc_sink;

  grd_vnc_sink_register ();

  element = gst_element_factory_make ("grdvncsink", NULL);

  vnc_sink = GRD_VNC_SINK (element);
  vnc_sink->session_vnc = session_vnc;
  g_signal_connect_object (session_vnc, "stopped",
                           G_CALLBACK (on_session_stopped),
                           vnc_sink,
                           0);

  return element;
}

static void
grd_vnc_sink_finalize (GObject *object)
{
  GrdVncSink *vnc_sink = GRD_VNC_SINK (object);

  g_mutex_clear (&vnc_sink->lock);

  G_OBJECT_CLASS (grd_vnc_sink_parent_class)->finalize (object);
}

static void
grd_vnc_sink_init (GrdVncSink *vnc_sink)
{
  const char *video_format_string;

  g_mutex_init (&vnc_sink->lock);

  video_format_string = gst_video_format_to_string (GST_VIDEO_FORMAT_RGBx);
  vnc_sink->caps =
    gst_caps_new_simple ("video/x-raw",
                         "format", G_TYPE_STRING, video_format_string,
                         "width", GST_TYPE_INT_RANGE, 1, G_MAXINT,
                         "height", GST_TYPE_INT_RANGE, 1, G_MAXINT,
                         "framerate", GST_TYPE_FRACTION_RANGE, 0, 1, G_MAXINT, 1,
                         NULL);
}

static void
grd_vnc_sink_class_init (GrdVncSinkClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GstElementClass *element_class = GST_ELEMENT_CLASS (klass);
  GstBaseSinkClass *base_sink_class = GST_BASE_SINK_CLASS (klass);

  static GstStaticPadTemplate sink_template =
    GST_STATIC_PAD_TEMPLATE ("sink",
                             GST_PAD_SINK,
                             GST_PAD_ALWAYS,
                             GST_STATIC_CAPS_ANY);

  gst_element_class_add_pad_template (element_class,
                                      gst_static_pad_template_get (&sink_template));
  gst_element_class_set_details_simple (element_class,
                                        "GrdVncSink",
                                        "Sink",
                                        "Remote desktop screen pipeline VNC sink",
                                        "Jonas Ådahl <jadahl@redhat.com>");

  object_class->finalize = grd_vnc_sink_finalize;

  base_sink_class->get_caps = grd_vnc_sink_get_caps;
  base_sink_class->set_caps = grd_vnc_sink_set_caps;
  base_sink_class->render = grd_vnc_sink_render;
}
