/*
 * Copyright (C) 2018 Red Hat Inc.
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

#include "grd-vnc-pipewire-stream.h"

#include <pipewire/pipewire.h>
#include <spa/param/props.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <sys/mman.h>

enum
{
  CLOSED,

  N_SIGNALS
};

guint signals[N_SIGNALS];

typedef struct _GrdSpaType
{
  struct spa_type_media_type media_type;
  struct spa_type_media_subtype media_subtype;
  struct spa_type_format_video format_video;
  struct spa_type_video_format video_format;
} GrdSpaType;

typedef struct _GrdPipeWireSource
{
  GSource base;

  struct pw_loop *pipewire_loop;
} GrdPipeWireSource;

struct _GrdVncPipeWireStream
{
  GObject parent;

  GrdSessionVnc *session;

  GrdPipeWireSource *pipewire_source;
  struct pw_core *pipewire_core;
  struct pw_type *pipewire_type;

  struct pw_remote *pipewire_remote;
  struct spa_hook pipewire_remote_listener;

  GrdSpaType spa_type;

  struct pw_stream *pipewire_stream;
  struct spa_hook pipewire_stream_listener;

  uint32_t src_node_id;
  char *src_node_id_string;

  struct spa_video_info_raw spa_format;
};

G_DEFINE_TYPE (GrdVncPipeWireStream, grd_vnc_pipewire_stream,
               G_TYPE_OBJECT)

static void
init_spa_type (GrdSpaType          *type,
               struct spa_type_map *map)
{
  spa_type_media_type_map (map, &type->media_type);
  spa_type_media_subtype_map (map, &type->media_subtype);
  spa_type_format_video_map (map, &type->format_video);
  spa_type_video_format_map (map, &type->video_format);
}

static gboolean
pipewire_loop_source_prepare (GSource *base,
                              int     *timeout)
{
  *timeout = -1;
  return FALSE;
}

static gboolean
pipewire_loop_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;
  int result;

  result = pw_loop_iterate (pipewire_source->pipewire_loop, 0);
  if (result < 0)
    g_warning ("pipewire_loop_iterate failed: %s", spa_strerror (result));

  return TRUE;
}

static void
pipewire_loop_source_finalize (GSource *source)
{
  GrdPipeWireSource *pipewire_source = (GrdPipeWireSource *) source;

  pw_loop_leave (pipewire_source->pipewire_loop);
  pw_loop_destroy (pipewire_source->pipewire_loop);
}

static GSourceFuncs pipewire_source_funcs =
{
  pipewire_loop_source_prepare,
  NULL,
  pipewire_loop_source_dispatch,
  pipewire_loop_source_finalize
};

static GrdPipeWireSource *
create_pipewire_source (void)
{
  GrdPipeWireSource *pipewire_source;

  pipewire_source =
    (GrdPipeWireSource *) g_source_new (&pipewire_source_funcs,
                                        sizeof (GrdPipeWireSource));
  pipewire_source->pipewire_loop = pw_loop_new (NULL);
  if (!pipewire_source->pipewire_loop)
    {
      g_source_destroy ((GSource *) pipewire_source);
      return NULL;
    }

  g_source_add_unix_fd (&pipewire_source->base,
                        pw_loop_get_fd (pipewire_source->pipewire_loop),
                        G_IO_IN | G_IO_ERR);

  pw_loop_enter (pipewire_source->pipewire_loop);
  g_source_attach (&pipewire_source->base, NULL);

  return pipewire_source;
}

static void
on_stream_state_changed (void                 *user_data,
                         enum pw_stream_state  old,
                         enum pw_stream_state  state,
                         const char           *error)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);

  g_debug ("Pipewire stream state changed from %s to %s",
           pw_stream_state_as_string (old),
           pw_stream_state_as_string (state));

  switch (state)
    {
    case PW_STREAM_STATE_ERROR:
      g_warning ("PipeWire stream error: %s", error);
      break;
    case PW_STREAM_STATE_CONFIGURE:
      pw_stream_set_active (stream->pipewire_stream, true);
      break;
    case PW_STREAM_STATE_UNCONNECTED:
    case PW_STREAM_STATE_CONNECTING:
    case PW_STREAM_STATE_READY:
    case PW_STREAM_STATE_PAUSED:
    case PW_STREAM_STATE_STREAMING:
      break;
    }
}

static void
on_stream_format_changed (void           *user_data,
                          struct spa_pod *format)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  struct pw_type *pipewire_type = stream->pipewire_type;
  struct spa_type_param_buffers *param_buffers = &pipewire_type->param_buffers;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  int width;
  int height;
  int stride;
  int size;
  struct spa_pod *params[2];

  if (!format)
    {
      pw_stream_finish_format (stream->pipewire_stream, 0, NULL, 0);
      return;
    }

  spa_format_video_raw_parse (format,
                              &stream->spa_format,
                              &stream->spa_type.format_video);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));

  width = stream->spa_format.size.width;
  height = stream->spa_format.size.height;

  grd_session_vnc_resize_framebuffer (stream->session, width, height);

  stride = grd_session_vnc_get_framebuffer_stride (stream->session);
  size = stride * height;

  params[0] = spa_pod_builder_object (
    &pod_builder,
    pipewire_type->param.idBuffers, param_buffers->Buffers,
    ":", param_buffers->size, "i", size,
    ":", param_buffers->stride, "i", stride,
    ":", param_buffers->buffers, "iru", 2, SPA_POD_PROP_MIN_MAX(1, 8));

  params[1] = spa_pod_builder_object (
    &pod_builder,
    pipewire_type->param.idMeta, pipewire_type->param_meta.Meta,
    ":", pipewire_type->param_meta.type, "I", pipewire_type->meta.Header,
    ":", pipewire_type->param_meta.size, "i", sizeof(struct spa_meta_header));

  pw_stream_finish_format (stream->pipewire_stream, 0, params, 2);
}

static int
do_render (struct spa_loop *loop,
           bool             async,
           uint32_t         seq,
           const void      *data,
           size_t           size,
           void            *user_data)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  struct spa_buffer *buffer = ((struct spa_buffer **) data)[0];
  uint8_t *map;
  void *src_data;

  if (buffer->datas[0].type == stream->pipewire_type->data.MemFd ||
      buffer->datas[0].type == stream->pipewire_type->data.DmaBuf)
    {
      map = mmap (NULL, buffer->datas[0].maxsize + buffer->datas[0].mapoffset,
                  PROT_READ, MAP_PRIVATE, buffer->datas[0].fd, 0);
      src_data = SPA_MEMBER (map, buffer->datas[0].mapoffset, uint8_t);
    }
  else if (buffer->datas[0].type == stream->pipewire_type->data.MemPtr)
    {
      map = NULL;
      src_data = buffer->datas[0].data;
    }
  else
    {
      return -EINVAL;
    }

  grd_session_vnc_draw_buffer (stream->session, src_data);

  if (map)
    munmap (map, buffer->datas[0].maxsize + buffer->datas[0].mapoffset);

  return 0;
}

static void
on_stream_new_buffer (void     *user_data,
                      uint32_t  id)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  struct spa_buffer *buffer;

  buffer = pw_stream_peek_buffer (stream->pipewire_stream, id);

  pw_loop_invoke (stream->pipewire_source->pipewire_loop, do_render,
                  SPA_ID_INVALID, &buffer, sizeof (struct spa_buffer *),
                  true, stream);

  pw_stream_recycle_buffer (stream->pipewire_stream, id);
}

static const struct pw_stream_events stream_events = {
  PW_VERSION_STREAM_EVENTS,
  .state_changed = on_stream_state_changed,
  .format_changed = on_stream_format_changed,
  .new_buffer = on_stream_new_buffer,
};

static gboolean
connect_to_stream (GrdVncPipeWireStream  *stream,
                   GError               **error)
{
  GrdSpaType *spa_type = &stream->spa_type;
  struct pw_stream *pipewire_stream;
  uint8_t params_buffer[1024];
  struct spa_pod_builder pod_builder;
  struct spa_rectangle min_rect;
  struct spa_rectangle max_rect;
  struct spa_fraction min_framerate;
  struct spa_fraction max_framerate;
  const struct spa_pod *params[1];
  int ret;

  pipewire_stream = pw_stream_new (stream->pipewire_remote,
                                   "grd-vnc-pipewire-stream",
                                   NULL);

  pod_builder = SPA_POD_BUILDER_INIT (params_buffer, sizeof (params_buffer));
  spa_pod_builder_push_object (&pod_builder,
                               stream->pipewire_type->param.idEnumFormat,
                               stream->pipewire_type->spa_format);
  spa_pod_builder_id (&pod_builder, stream->spa_type.media_type.video);
  spa_pod_builder_id (&pod_builder, stream->spa_type.media_subtype.raw);

  spa_pod_builder_push_prop (&pod_builder,
                             stream->spa_type.format_video.format,
                             SPA_POD_PROP_RANGE_ENUM);
  spa_pod_builder_id (&pod_builder, stream->spa_type.video_format.BGRx);

  min_rect = SPA_RECTANGLE (1, 1);
  max_rect = SPA_RECTANGLE (INT32_MAX, INT32_MAX);
  min_framerate = SPA_FRACTION (1, 1);
  max_framerate = SPA_FRACTION (30, 1);

  spa_pod_builder_pop (&pod_builder);
  spa_pod_builder_add (
    &pod_builder,
    ":", spa_type->format_video.size, "Rru", &min_rect,
                                             SPA_POD_PROP_MIN_MAX (&min_rect,
                                                                   &max_rect),
    ":", spa_type->format_video.framerate, "F", &SPA_FRACTION (0, 1),
    ":", spa_type->format_video.max_framerate, "Fru", &max_framerate,
                                                      SPA_POD_PROP_MIN_MAX (&min_framerate,
                                                                            &max_framerate),
    NULL);
  params[0] = spa_pod_builder_pop (&pod_builder);

  stream->pipewire_stream = pipewire_stream;

  pw_stream_add_listener (pipewire_stream,
                          &stream->pipewire_stream_listener,
                          &stream_events,
                          stream);

  ret = pw_stream_connect (stream->pipewire_stream,
                           PW_DIRECTION_INPUT,
                           stream->src_node_id_string,
                           PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_INACTIVE,
                           params, 1);
  if (ret < 0)
    {
      g_set_error (error, G_IO_ERROR, g_io_error_from_errno (-ret),
                   strerror (-ret));
      return FALSE;
    }

  return TRUE;
}

static void
on_state_changed (void                 *user_data,
                  enum pw_remote_state  old,
                  enum pw_remote_state  state,
                  const char           *error_message)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (user_data);
  g_autoptr (GError) error = NULL;

  g_debug ("Pipewire remote state changed from %s to %s",
           pw_remote_state_as_string (old),
           pw_remote_state_as_string (state));

  switch (state)
    {
    case PW_REMOTE_STATE_ERROR:
      g_warning ("pipewire remote error: %s\n", error_message);
      g_signal_emit (stream, signals[CLOSED], 0);
      break;
    case PW_REMOTE_STATE_CONNECTED:
      if (!connect_to_stream (stream, &error))
        {
          g_warning ("Failed to connect to stream: %s", error->message);
          g_signal_emit (stream, signals[CLOSED], 0);
        }
      break;
    case PW_REMOTE_STATE_UNCONNECTED:
    case PW_REMOTE_STATE_CONNECTING:
      break;
    }
}

static const struct pw_remote_events remote_events = {
  PW_VERSION_REMOTE_EVENTS,
  .state_changed = on_state_changed,
};

GrdVncPipeWireStream *
grd_vnc_pipewire_stream_new (GrdSessionVnc  *session_vnc,
                             uint32_t        src_node_id,
                             GError        **error)
{
  GrdVncPipeWireStream *stream;
  static gboolean is_pipewire_initialized = FALSE;

  if (!is_pipewire_initialized)
    {
      pw_init (NULL, NULL);
      is_pipewire_initialized = TRUE;
    }

  stream = g_object_new (GRD_TYPE_VNC_PIPEWIRE_STREAM, NULL);
  stream->session = session_vnc;
  stream->src_node_id = src_node_id;
  stream->src_node_id_string = g_strdup_printf ("%u", src_node_id);

  stream->pipewire_source = create_pipewire_source ();
  if (!stream->pipewire_source)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create PipeWire source");
      return NULL;
    }

  stream->pipewire_core = pw_core_new (stream->pipewire_source->pipewire_loop,
                                       NULL);
  if (!stream->pipewire_core)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create pipewire core");
      return NULL;
    }

  stream->pipewire_remote = pw_remote_new (stream->pipewire_core, NULL, 0);
  if (!stream->pipewire_remote)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't creat pipewire remote");
      return NULL;
    }

  pw_remote_add_listener (stream->pipewire_remote,
                          &stream->pipewire_remote_listener,
                          &remote_events,
                          stream);

  stream->pipewire_type = pw_core_get_type (stream->pipewire_core);
  init_spa_type (&stream->spa_type, stream->pipewire_type->map);

  if (pw_remote_connect (stream->pipewire_remote) != 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Couldn't connect pipewire remote");
      return NULL;
    }

  return stream;
}

static void
grd_vnc_pipewire_stream_finalize (GObject *object)
{
  GrdVncPipeWireStream *stream = GRD_VNC_PIPEWIRE_STREAM (object);

  g_clear_pointer (&stream->src_node_id_string, g_free);
  g_clear_pointer (&stream->pipewire_stream,
                   (GDestroyNotify) pw_stream_destroy);
  g_clear_pointer (&stream->pipewire_remote,
                   (GDestroyNotify) pw_remote_destroy);
  g_clear_pointer (&stream->pipewire_core,
                   (GDestroyNotify) pw_core_destroy);
  g_clear_pointer (&stream->pipewire_source,
                   (GDestroyNotify) g_source_destroy);

  G_OBJECT_CLASS (grd_vnc_pipewire_stream_parent_class)->finalize (object);
}

static void
grd_vnc_pipewire_stream_init (GrdVncPipeWireStream *stream)
{
}

static void
grd_vnc_pipewire_stream_class_init (GrdVncPipeWireStreamClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->finalize = grd_vnc_pipewire_stream_finalize;

  signals[CLOSED] = g_signal_new ("closed",
                                  G_TYPE_FROM_CLASS (klass),
                                  G_SIGNAL_RUN_LAST,
                                  0,
                                  NULL, NULL, NULL,
                                  G_TYPE_NONE, 0);
}
