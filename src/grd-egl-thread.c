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

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <gio/gio.h>

struct _GrdEglThread
{
  GThread *thread;
  GMutex mutex;
  GCond cond;

  GAsyncQueue *task_queue;

  GHashTable *modifiers;

  struct
  {
    gboolean initialized;
    GError *error;

    GMainContext *main_context;
    GMainLoop *main_loop;

    EGLDisplay egl_display;
    EGLContext egl_context;

    GSource *egl_thread_source;
  } impl;
};

typedef struct _GrdEglTask
{
  GFunc func;
  GDestroyNotify destroy;

  GMainContext *callback_context;
  GrdEglThreadCallback callback;
  gpointer callback_user_data;
  GDestroyNotify callback_destroy;
} GrdEglTask;

typedef struct _GrdEglTaskCustom
{
  GrdEglTask base;

  GrdEglThreadCustomFunc custom_func;
  gpointer user_data;
} GrdEglTaskCustom;

typedef struct _GrdEglTaskSync
{
  GrdEglTask base;
} GrdEglTaskSync;

typedef struct _GrdEglTaskUpload
{
  GrdEglTask base;

  GLuint pbo;

  uint32_t height;
  uint32_t stride;
  uint8_t *src_data;

  GrdEglThreadAllocBufferFunc allocate_func;
  gpointer allocate_user_data;
  GDestroyNotify allocate_user_data_destroy;

  GrdEglThreadCustomFunc realize_func;
  gpointer realize_user_data;
  GDestroyNotify realize_user_data_destroy;
} GrdEglTaskUpload;

typedef struct _GrdEglTaskDeallocateMemory
{
  GrdEglTask base;

  GLuint pbo;

  GrdEglThreadDeallocBufferFunc deallocate_func;
  gpointer deallocate_user_data;
} GrdEglTaskDeallocateMemory;

typedef struct _GrdEglTaskAllocateMemory
{
  GrdEglTask base;

  uint32_t height;
  uint32_t stride;

  GrdEglThreadAllocBufferFunc allocate_func;
  gpointer allocate_user_data;
} GrdEglTaskAllocateMemory;

typedef struct _GrdEglTaskDownload
{
  GrdEglTask base;

  GLuint pbo;
  uint32_t pbo_height;
  uint32_t pbo_stride;

  GrdEglThreadImportIface iface;
  gpointer import_user_data;
  GDestroyNotify import_destroy_notify;

  uint8_t *dst_data;
  int dst_row_width;

  uint32_t format;
  unsigned int width;
  unsigned int height;
  uint32_t n_planes;
  int *fds;
  uint32_t *strides;
  uint32_t *offsets;
  uint64_t *modifiers;
} GrdEglTaskDownload;

typedef void (* GrdEglFrameReady) (uint8_t  *data,
                                   gpointer  user_data);

static gboolean
is_hardware_accelerated (void)
{
  const char *renderer;
  gboolean is_software;

  renderer = (const char *) glGetString (GL_RENDERER);
  g_assert_cmpuint (glGetError (), ==, GL_NO_ERROR);
  if (!renderer)
    {
      g_warning ("OpenGL driver returned NULL as the renderer, "
                 "something is wrong");
      return FALSE;
    }

  is_software = (strstr (renderer, "llvmpipe") ||
                 strstr (renderer, "softpipe") ||
                 strstr (renderer, "software rasterizer") ||
                 strstr (renderer, "Software Rasterizer") ||
                 strstr (renderer, "SWR"));
  return !is_software;
}

static gboolean
query_format_modifiers (GrdEglThread  *egl_thread,
                        EGLDisplay     egl_display,
                        GError       **error)
{
  EGLint n_formats = 0;
  g_autofree EGLint *formats = NULL;
  EGLint i;

  if (!eglQueryDmaBufFormatsEXT (egl_display, 0, NULL, &n_formats))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query number of DMA buffer formats");
      return FALSE;
    }

  if (n_formats == 0)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No available DMA buffer formats");
      return FALSE;
    }

  formats = g_new0 (EGLint, n_formats);
  if (!eglQueryDmaBufFormatsEXT (egl_display, n_formats, formats, &n_formats))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to query DMA buffer formats");
      return FALSE;
    }

  for (i = 0; i < n_formats; i++)
    {
      EGLint n_modifiers;
      GArray *modifiers;

      if (!eglQueryDmaBufModifiersEXT (egl_display,
                                       formats[i], 0, NULL, NULL,
                                       &n_modifiers))
        {
          g_warning ("Failed to query number of modifiers for format %d",
                     formats[0]);
          continue;
        }

      if (n_modifiers == 0)
        {
          g_debug ("No modifiers available for format %d", formats[0]);
          continue;
        }

      modifiers = g_array_sized_new (FALSE, FALSE,
                                     sizeof (EGLuint64KHR),
                                     n_modifiers);
      g_array_set_size (modifiers, n_modifiers);
      if (!eglQueryDmaBufModifiersEXT (egl_display,
                                       formats[i], n_modifiers,
                                       (EGLuint64KHR *) modifiers->data,
                                       NULL,
                                       &n_modifiers))
        {
          g_warning ("Failed to query modifiers for format %d",
                     formats[0]);
          continue;
        }

      g_hash_table_insert (egl_thread->modifiers,
                           GINT_TO_POINTER (formats[i]),
                           modifiers);
    }

  return TRUE;
}

static gboolean
initialize_egl_context (GrdEglThread  *egl_thread,
                        EGLenum        egl_platform,
                        GError       **error)
{
  EGLDisplay egl_display;
  EGLint major, minor;
  EGLint *attrs;
  EGLContext egl_context;

  g_clear_error (error);

  egl_display = eglGetPlatformDisplayEXT (egl_platform, NULL, NULL);
  if (egl_display == EGL_NO_DISPLAY)
    {
      int code = egl_platform == EGL_PLATFORM_DEVICE_EXT ? G_IO_ERROR_NOT_SUPPORTED
                                                         : G_IO_ERROR_FAILED;
      g_set_error (error, G_IO_ERROR, code,
                   "Failed to get EGL display");
      return FALSE;
    }

  if (!eglInitialize (egl_display, &major, &minor))
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to initialize EGL display");
      return FALSE;
    }

  if (!eglBindAPI (EGL_OPENGL_ES_API))
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to bind OpenGL ES API");
      return FALSE;
    }

  if (egl_platform == EGL_PLATFORM_WAYLAND_EXT)
    {
      if (!epoxy_has_egl_extension (egl_display, "EGL_MESA_configless_context"))
        {
          eglTerminate (egl_display);
          g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                       "Missing extension 'EGL_MESA_configless_context'");
          return FALSE;
        }
    }

  if (!epoxy_has_egl_extension (egl_display,
                                "EGL_EXT_image_dma_buf_import_modifiers"))
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Missing extension 'EGL_EXT_image_dma_buf_import_modifiers'");
      return FALSE;
    }

  attrs = (EGLint[]) {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  egl_context = eglCreateContext (egl_display,
                                  EGL_NO_CONFIG_KHR,
                                  EGL_NO_CONTEXT,
                                  attrs);
  if (egl_context == EGL_NO_CONTEXT)
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create EGL context");
      return FALSE;
    }

  eglMakeCurrent (egl_display,
                  EGL_NO_SURFACE,
                  EGL_NO_SURFACE,
                  egl_context);

  if (!query_format_modifiers (egl_thread, egl_display, error))
    {
      eglDestroyContext (egl_display, egl_context);
      eglTerminate (egl_display);
      return FALSE;
    }

  if (g_hash_table_size (egl_thread->modifiers) == 0)
    {
      eglDestroyContext (egl_display, egl_context);
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "No DMA buffer modifiers / format pair found");
      return FALSE;
    }

  if (!is_hardware_accelerated ())
    {
      eglDestroyContext (egl_display, egl_context);
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "EGL context not hardware accelerated");
      return FALSE;
    }

  egl_thread->impl.egl_display = egl_display;
  egl_thread->impl.egl_context = egl_context;

  return TRUE;
}

static gboolean
grd_egl_init_in_impl (GrdEglThread  *egl_thread,
                      GError       **error)
{
  gboolean initialized;

  if (!epoxy_has_egl_extension (NULL, "EGL_EXT_platform_base"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing extension 'EGL_EXT_platform_base'");
      return FALSE;
    }

  initialized = initialize_egl_context (egl_thread,
                                        EGL_PLATFORM_WAYLAND_EXT,
                                        error);
  if (initialized)
    {
      g_debug ("EGL context initialized with platform "
               "EGL_PLATFORM_WAYLAND_EXT");
    }
  else
    {
      initialized = initialize_egl_context (egl_thread,
                                            EGL_PLATFORM_DEVICE_EXT,
                                            error);
      if (initialized)
        {
          g_debug ("EGL context initialized with platform "
                   "EGL_PLATFORM_DEVICE_EXT");
        }
    }

  return initialized;
}

static void
grd_egl_task_free (GrdEglTask *task)
{
  if (task->callback_destroy)
    g_clear_pointer (&task->callback_user_data, task->callback_destroy);
  g_free (task);
}

static void
grd_egl_task_download_free (GrdEglTask *task_base)
{
  GrdEglTaskDownload *task = (GrdEglTaskDownload *) task_base;

  g_free (task->fds);
  g_free (task->strides);
  g_free (task->offsets);
  g_free (task->modifiers);

  if (task->import_destroy_notify)
    task->import_destroy_notify (task->import_user_data);

  grd_egl_task_free (task_base);
}

static void
grd_egl_task_upload_free (GrdEglTask *task_base)
{
  GrdEglTaskUpload *task = (GrdEglTaskUpload *) task_base;

  task->allocate_user_data_destroy (task->allocate_user_data);
  task->realize_user_data_destroy (task->realize_user_data);

  grd_egl_task_free (task_base);
}

static void
grd_thread_dispatch_in_impl (GrdEglThread *egl_thread)
{
  GrdEglTask *task;

  task = g_async_queue_try_pop (egl_thread->task_queue);
  if (!task)
    return;

  task->func (egl_thread, task);
  task->destroy (task);
}

typedef struct _EglTaskSource
{
  GSource base;
  GrdEglThread *egl_thread;
} EglTaskSource;

static gboolean
egl_task_source_prepare (GSource *source,
                         int     *timeout)
{
  EglTaskSource *task_source = (EglTaskSource *) source;
  GrdEglThread *egl_thread = task_source->egl_thread;

  g_assert (g_source_get_context (source) == egl_thread->impl.main_context);

  *timeout = -1;

  return g_async_queue_length (egl_thread->task_queue) > 0;
}

static gboolean
egl_task_source_check (GSource *source)
{
  EglTaskSource *task_source = (EglTaskSource *) source;
  GrdEglThread *egl_thread = task_source->egl_thread;

  g_assert (g_source_get_context (source) == egl_thread->impl.main_context);

  return g_async_queue_length (egl_thread->task_queue) > 0;
}

static gboolean
egl_task_source_dispatch (GSource     *source,
                          GSourceFunc  callback,
                          gpointer     user_data)
{
  EglTaskSource *task_source = (EglTaskSource *) source;
  GrdEglThread *egl_thread = task_source->egl_thread;

  g_assert (g_source_get_context (source) == egl_thread->impl.main_context);

  grd_thread_dispatch_in_impl (egl_thread);

  return G_SOURCE_CONTINUE;
}

static GSourceFuncs egl_task_source_funcs = {
  .prepare = egl_task_source_prepare,
  .check = egl_task_source_check,
  .dispatch = egl_task_source_dispatch,
};

static gpointer
grd_egl_thread_func (gpointer user_data)
{
  GrdEglThread *egl_thread = user_data;
  GError *error = NULL;

  egl_thread->impl.main_context = g_main_context_new ();

  if (!grd_egl_init_in_impl (egl_thread, &error))
    {
      g_propagate_error (&egl_thread->impl.error, error);
      g_mutex_lock (&egl_thread->mutex);
      egl_thread->impl.initialized = TRUE;
      g_cond_signal (&egl_thread->cond);
      g_mutex_unlock (&egl_thread->mutex);
      return NULL;
    }

  egl_thread->impl.egl_thread_source =
    g_source_new (&egl_task_source_funcs, sizeof (EglTaskSource));
  ((EglTaskSource *) egl_thread->impl.egl_thread_source)->egl_thread =
    egl_thread;
  g_source_set_name (egl_thread->impl.egl_thread_source, "EGL thread source");
  g_source_set_ready_time (egl_thread->impl.egl_thread_source, -1);
  g_source_attach (egl_thread->impl.egl_thread_source,
                   egl_thread->impl.main_context);
  egl_thread->task_queue = g_async_queue_new ();

  egl_thread->impl.main_loop = g_main_loop_new (egl_thread->impl.main_context,
                                                FALSE);
  g_mutex_lock (&egl_thread->mutex);
  egl_thread->impl.initialized = TRUE;
  g_cond_signal (&egl_thread->cond);
  g_mutex_unlock (&egl_thread->mutex);

  g_main_loop_run (egl_thread->impl.main_loop);

  g_source_destroy (egl_thread->impl.egl_thread_source);
  g_source_unref (egl_thread->impl.egl_thread_source);

  g_clear_pointer (&egl_thread->impl.main_loop, g_main_loop_unref);

  while (TRUE)
    {
      GrdEglTask *task;

      task = g_async_queue_try_pop (egl_thread->task_queue);
      if (!task)
        break;

      task->destroy (task);
    }
  g_async_queue_unref (egl_thread->task_queue);

  eglMakeCurrent (egl_thread->impl.egl_display,
                  EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
  eglDestroyContext (egl_thread->impl.egl_display,
                     egl_thread->impl.egl_context);
  eglTerminate (egl_thread->impl.egl_display);

  return NULL;
}

GrdEglThread *
grd_egl_thread_new (GError **error)
{
  GrdEglThread *egl_thread;

  egl_thread = g_new0 (GrdEglThread, 1);

  egl_thread->modifiers =
    g_hash_table_new_full (NULL, NULL,
                           NULL, (GDestroyNotify) g_array_unref);

  g_mutex_init (&egl_thread->mutex);
  g_cond_init (&egl_thread->cond);

  g_mutex_lock (&egl_thread->mutex);

  egl_thread->thread = g_thread_new ("GRD EGL thread",
                                     grd_egl_thread_func,
                                     egl_thread);

  while (!egl_thread->impl.initialized)
    g_cond_wait (&egl_thread->cond, &egl_thread->mutex);
  g_mutex_unlock (&egl_thread->mutex);

  if (egl_thread->impl.error)
    {
      g_propagate_error (error, egl_thread->impl.error);
      grd_egl_thread_free (egl_thread);
      return NULL;
    }

  return egl_thread;
}

static gboolean
quit_main_loop_in_impl (gpointer user_data)
{
  GrdEglThread *egl_thread = user_data;

  if (egl_thread->impl.main_loop)
    g_main_loop_quit (egl_thread->impl.main_loop);

  return G_SOURCE_REMOVE;
}

void
grd_egl_thread_free (GrdEglThread *egl_thread)
{
  GSource *source;

  source = g_idle_source_new ();
  g_source_set_callback (source, quit_main_loop_in_impl, egl_thread, NULL);
  g_source_attach (source, egl_thread->impl.main_context);
  g_source_unref (source);

  g_thread_join (egl_thread->thread);
  g_mutex_clear (&egl_thread->mutex);
  g_cond_clear (&egl_thread->cond);

  g_clear_pointer (&egl_thread->modifiers, g_hash_table_unref);
  g_clear_pointer (&egl_thread->impl.main_context, g_main_context_unref);

  g_free (egl_thread);
}

static EGLImageKHR
create_dmabuf_image (GrdEglThread   *egl_thread,
                     unsigned int    width,
                     unsigned int    height,
                     uint32_t        format,
                     uint32_t        n_planes,
                     const int      *fds,
                     const uint32_t *strides,
                     const uint32_t *offsets,
                     const uint64_t *modifiers)
{
  EGLint attribs[37];
  int atti = 0;
  EGLImageKHR egl_image;

  attribs[atti++] = EGL_WIDTH;
  attribs[atti++] = width;
  attribs[atti++] = EGL_HEIGHT;
  attribs[atti++] = height;
  attribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[atti++] = format;

  if (n_planes > 0)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE0_FD_EXT;
      attribs[atti++] = fds[0];
      attribs[atti++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
      attribs[atti++] = offsets[0];
      attribs[atti++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
      attribs[atti++] = strides[0];
      if (modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
          attribs[atti++] = modifiers[0] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
          attribs[atti++] = modifiers[0] >> 32;
        }
    }

  if (n_planes > 1)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE1_FD_EXT;
      attribs[atti++] = fds[1];
      attribs[atti++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
      attribs[atti++] = offsets[1];
      attribs[atti++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
      attribs[atti++] = strides[1];
      if (modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
          attribs[atti++] = modifiers[1] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
          attribs[atti++] = modifiers[1] >> 32;
        }
    }

  if (n_planes > 2)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE2_FD_EXT;
      attribs[atti++] = fds[2];
      attribs[atti++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
      attribs[atti++] = offsets[2];
      attribs[atti++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
      attribs[atti++] = strides[2];
      if (modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
          attribs[atti++] = modifiers[2] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
          attribs[atti++] = modifiers[2] >> 32;
        }
    }

  attribs[atti++] = EGL_NONE;
  g_assert (atti <= G_N_ELEMENTS (attribs));

  egl_image = eglCreateImageKHR (egl_thread->impl.egl_display,
                                 EGL_NO_CONTEXT,
                                 EGL_LINUX_DMA_BUF_EXT, NULL,
                                 attribs);
  if (egl_image == EGL_NO_IMAGE)
    {
      g_warning ("Failed to import DMA buffer as EGL image: %d",
                 eglGetError ());
    }

  return egl_image;
}

static gboolean
bind_egl_image (GrdEglThread *egl_thread,
                EGLImageKHR   egl_image,
                int           dst_row_width,
                GLuint       *tex,
                GLuint       *fbo)
{
  GLenum error;

  glGenTextures (1, tex);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glBindTexture (GL_TEXTURE_2D, *tex);
  glPixelStorei (GL_PACK_ROW_LENGTH, dst_row_width);
  glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, egl_image);
  error = glGetError ();
  if (error != GL_NO_ERROR)
    {
      g_warning ("[EGL Thread] Failed to bind DMA buf texture: %u", error);
      return FALSE;
    }

  if (!fbo)
    return TRUE;

  glGenFramebuffers (1, fbo);
  glBindFramebuffer (GL_FRAMEBUFFER, *fbo);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, *tex, 0);
  if (glCheckFramebufferStatus(GL_FRAMEBUFFER) !=
      GL_FRAMEBUFFER_COMPLETE)
    {
      g_warning ("Failed to bind DMA buf framebuffer");
      return FALSE;
    }

  return TRUE;
}

static void
read_pixels (uint8_t      *dst_data,
             unsigned int  width,
             unsigned int  height)
{
  glReadPixels (0, 0, width, height,
                GL_BGRA, GL_UNSIGNED_BYTE,
                dst_data);
}

static void
download_in_impl (gpointer data,
                  gpointer user_data)
{
  GrdEglThread *egl_thread = data;
  GrdEglTaskDownload *task = user_data;
  EGLImageKHR egl_image;
  gboolean success = FALSE;
  uint32_t buffer_size;
  GLuint tex = 0;
  GLuint fbo = 0;

  buffer_size = task->pbo_stride * task->pbo_height * sizeof (uint8_t);
  if (task->iface.allocate && !task->pbo)
    {
      GLuint pbo = 0;

      glGenBuffers (1, &pbo);
      glBindBuffer (GL_PIXEL_PACK_BUFFER, pbo);
      glBufferData (GL_PIXEL_PACK_BUFFER, buffer_size, NULL, GL_DYNAMIC_DRAW);
      glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

      if (!task->iface.allocate (task->import_user_data, pbo))
        {
          g_warning ("[EGL Thread] Failed to allocate GL resources");
          glDeleteBuffers (1, &pbo);
          goto out;
        }

      g_debug ("[EGL Thread] Allocating GL resources was successful");
      task->pbo = pbo;
    }

  egl_image =
    create_dmabuf_image (egl_thread,
                         task->width,
                         task->height,
                         task->format,
                         task->n_planes,
                         task->fds,
                         task->strides,
                         task->offsets,
                         task->modifiers);
  if (egl_image == EGL_NO_IMAGE)
    goto out;

  if (!bind_egl_image (egl_thread, egl_image, task->dst_row_width, &tex,
                       task->dst_data ? &fbo : NULL))
    goto out;

  if (task->iface.realize)
    {
      GLenum error;

      glBindBuffer (GL_PIXEL_PACK_BUFFER, task->pbo);
      glBufferData (GL_PIXEL_PACK_BUFFER, buffer_size, NULL, GL_DYNAMIC_DRAW);
      glReadPixels (0, 0, task->width, task->height,
                    GL_BGRA, GL_UNSIGNED_BYTE, NULL);
      error = glGetError ();
      if (error != GL_NO_ERROR)
        {
          g_warning ("[EGL Thread] Failed to update buffer data: %u", error);
          goto out;
        }

      glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

      if (!task->iface.realize (task->import_user_data))
        goto out;
    }

  if (task->dst_data)
    read_pixels (task->dst_data, task->width, task->height);

  success = TRUE;

out:
  glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

  if (fbo)
    {
      glBindFramebuffer (GL_FRAMEBUFFER, 0);
      glDeleteFramebuffers (1, &fbo);
    }
  if (tex)
    {
      glBindTexture (GL_TEXTURE_2D, 0);
      glDeleteTextures (1, &tex);
    }

  if (egl_image != EGL_NO_IMAGE)
    eglDestroyImageKHR (egl_thread->impl.egl_display, egl_image);

  task->base.callback (success, task->base.callback_user_data);
}

static void
allocate_in_impl (gpointer data,
                  gpointer user_data)
{
  GrdEglTaskAllocateMemory *task = user_data;
  gboolean success = FALSE;
  uint32_t buffer_size;
  GLuint pbo = 0;

  buffer_size = task->stride * task->height * sizeof (uint8_t);

  glGenBuffers (1, &pbo);
  glBindBuffer (GL_PIXEL_PACK_BUFFER, pbo);
  glBufferData (GL_PIXEL_PACK_BUFFER, buffer_size, NULL, GL_DYNAMIC_DRAW);
  glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

  if (task->allocate_func (task->allocate_user_data, pbo))
    success = TRUE;

  if (!success && pbo)
    {
      glDeleteBuffers (1, &pbo);
      pbo = 0;
    }

  task->base.callback (success, task->base.callback_user_data);
}

static void
deallocate_in_impl (gpointer data,
                    gpointer user_data)
{
  GrdEglTaskDeallocateMemory *task = user_data;

  task->deallocate_func (task->deallocate_user_data);

  if (task->pbo)
    glDeleteBuffers (1, &task->pbo);

  if (task->base.callback)
    task->base.callback (TRUE, task->base.callback_user_data);
}

static void
upload_in_impl (gpointer data,
                gpointer user_data)
{
  GrdEglTaskUpload *task = user_data;
  gboolean success = FALSE;
  uint32_t buffer_size;
  GLenum error;

  buffer_size = task->stride * task->height * sizeof (uint8_t);

  if (!task->pbo)
    {
      GLuint pbo = 0;

      glGenBuffers (1, &pbo);
      glBindBuffer (GL_PIXEL_PACK_BUFFER, pbo);
      glBufferData (GL_PIXEL_PACK_BUFFER, buffer_size, NULL, GL_DYNAMIC_DRAW);
      glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

      if (!task->allocate_func (task->allocate_user_data, pbo))
        {
          g_warning ("[EGL Thread] Failed to allocate GL resources");
          glDeleteBuffers (1, &pbo);
          goto out;
        }

      g_debug ("[EGL Thread] Allocating GL resources was successful");
      task->pbo = pbo;
    }

  glBindBuffer (GL_PIXEL_PACK_BUFFER, task->pbo);
  glBufferData (GL_PIXEL_PACK_BUFFER, buffer_size, NULL, GL_DYNAMIC_DRAW);
  glBufferSubData (GL_PIXEL_PACK_BUFFER, 0, buffer_size, task->src_data);
  error = glGetError ();
  if (error != GL_NO_ERROR)
    {
      g_warning ("[EGL Thread] Failed to update buffer data: %u", error);
      goto out;
    }

  if (!task->realize_func (task->realize_user_data))
    goto out;

  success = TRUE;

out:
  glBindBuffer (GL_PIXEL_PACK_BUFFER, 0);

  task->base.callback (success, task->base.callback_user_data);
}

static void
sync_in_impl (gpointer data,
              gpointer user_data)
{
  GrdEglTaskSync *task = user_data;

  task->base.callback (TRUE, task->base.callback_user_data);
}

static void
run_custom_task_in_impl (gpointer data,
                         gpointer user_data)
{
  GrdEglTaskCustom *task = user_data;
  gboolean success;

  success = task->custom_func (task->user_data);

  if (task->base.callback)
    task->base.callback (success, task->base.callback_user_data);
}

void
grd_egl_thread_download (GrdEglThread                  *egl_thread,
                         uint32_t                       pbo,
                         uint32_t                       pbo_height,
                         uint32_t                       pbo_stride,
                         const GrdEglThreadImportIface *iface,
                         gpointer                       import_user_data,
                         GDestroyNotify                 import_destroy_notify,
                         uint8_t                       *dst_data,
                         int                            dst_row_width,
                         uint32_t                       format,
                         unsigned int                   width,
                         unsigned int                   height,
                         uint32_t                       n_planes,
                         const int                     *fds,
                         const uint32_t                *strides,
                         const uint32_t                *offsets,
                         const uint64_t                *modifiers,
                         GrdEglThreadCallback           callback,
                         gpointer                       user_data,
                         GDestroyNotify                 destroy)
{
  GrdEglTaskDownload *task;

  task = g_new0 (GrdEglTaskDownload, 1);

  task->pbo = pbo;
  task->pbo_height = pbo_height;
  task->pbo_stride = pbo_stride;

  task->iface = iface ? *iface : (GrdEglThreadImportIface) {};
  task->import_user_data = import_user_data;
  task->import_destroy_notify = import_destroy_notify;

  task->dst_data = dst_data;
  task->dst_row_width = dst_row_width;

  task->format = format;
  task->width = width;
  task->height = height;
  task->n_planes = n_planes;
  task->fds = g_memdup2 (fds, n_planes * sizeof (int));
  task->strides = g_memdup2 (strides, n_planes * sizeof (uint32_t));
  task->offsets = g_memdup2 (offsets, n_planes * sizeof (uint32_t));
  task->modifiers = g_memdup2 (modifiers, n_planes * sizeof (uint64_t));

  task->base.func = download_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_download_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

void
grd_egl_thread_allocate (GrdEglThread                *egl_thread,
                         uint32_t                     height,
                         uint32_t                     stride,
                         GrdEglThreadAllocBufferFunc  allocate_func,
                         gpointer                     allocate_user_data,
                         GrdEglThreadCallback         callback,
                         gpointer                     user_data,
                         GDestroyNotify               destroy)
{
  GrdEglTaskAllocateMemory *task;

  task = g_new0 (GrdEglTaskAllocateMemory, 1);

  task->height = height;
  task->stride = stride;
  task->allocate_func = allocate_func;
  task->allocate_user_data = allocate_user_data;

  task->base.func = allocate_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

void
grd_egl_thread_deallocate (GrdEglThread                  *egl_thread,
                           uint32_t                       pbo,
                           GrdEglThreadDeallocBufferFunc  deallocate_func,
                           gpointer                       deallocate_user_data,
                           GrdEglThreadCallback           callback,
                           gpointer                       user_data,
                           GDestroyNotify                 destroy)
{
  GrdEglTaskDeallocateMemory *task;

  task = g_new0 (GrdEglTaskDeallocateMemory, 1);

  task->pbo = pbo;

  task->deallocate_func = deallocate_func;
  task->deallocate_user_data = deallocate_user_data;

  task->base.func = deallocate_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

void
grd_egl_thread_upload (GrdEglThread                *egl_thread,
                       uint32_t                     pbo,
                       uint32_t                     height,
                       uint32_t                     stride,
                       uint8_t                     *src_data,
                       GrdEglThreadAllocBufferFunc  allocate_func,
                       gpointer                     allocate_user_data,
                       GDestroyNotify               allocate_user_data_destroy,
                       GrdEglThreadCustomFunc       realize_func,
                       gpointer                     realize_user_data,
                       GDestroyNotify               realize_user_data_destroy,
                       GrdEglThreadCallback         callback,
                       gpointer                     user_data,
                       GDestroyNotify               destroy)
{
  GrdEglTaskUpload *task;

  task = g_new0 (GrdEglTaskUpload, 1);

  task->pbo = pbo;

  task->height = height;
  task->stride = stride;
  task->src_data = src_data;

  task->allocate_func = allocate_func;
  task->allocate_user_data = allocate_user_data;
  task->allocate_user_data_destroy = allocate_user_data_destroy;

  task->realize_func = realize_func;
  task->realize_user_data = realize_user_data;
  task->realize_user_data_destroy = realize_user_data_destroy;

  task->base.func = upload_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_upload_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

void
grd_egl_thread_sync (GrdEglThread         *egl_thread,
                     GrdEglThreadCallback  callback,
                     gpointer              user_data,
                     GDestroyNotify        destroy)
{
  GrdEglTaskSync *task;

  task = g_new0 (GrdEglTaskSync, 1);

  task->base.func = sync_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

void
grd_egl_thread_run_custom_task (GrdEglThread           *egl_thread,
                                GrdEglThreadCustomFunc  custom_func,
                                gpointer                custom_func_data,
                                GrdEglThreadCallback    callback,
                                gpointer                user_data,
                                GDestroyNotify          destroy)
{
  GrdEglTaskCustom *task;

  task = g_new0 (GrdEglTaskCustom, 1);

  task->custom_func = custom_func;
  task->user_data = custom_func_data;

  task->base.func = run_custom_task_in_impl;
  task->base.destroy = (GDestroyNotify) grd_egl_task_free;
  task->base.callback = callback;
  task->base.callback_user_data = user_data;
  task->base.callback_destroy = destroy;

  g_async_queue_push (egl_thread->task_queue, task);
  g_main_context_wakeup (egl_thread->impl.main_context);
}

gboolean
grd_egl_thread_get_modifiers_for_format (GrdEglThread  *egl_thread,
                                         uint32_t       format,
                                         int           *out_n_modifiers,
                                         uint64_t     **out_modifiers)
{
  GArray *modifiers;

  modifiers = g_hash_table_lookup (egl_thread->modifiers,
                                   GINT_TO_POINTER (format));
  if (!modifiers)
    return FALSE;

  *out_n_modifiers = modifiers->len;
  *out_modifiers = g_memdup2 (modifiers->data,
                              sizeof (uint64_t) * modifiers->len);
  return TRUE;
}
