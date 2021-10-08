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

  struct
  {
    gboolean initialized;
    GError *error;

    GMainContext *main_context;
    GMainLoop *main_loop;

    EGLDisplay egl_display;
    EGLContext egl_context;
  } impl;
};

static gboolean
grd_egl_init_in_impl (GrdEglThread  *egl_thread,
                      GError       **error)
{
  EGLDisplay egl_display;
  EGLint major, minor;
  EGLContext egl_context;

  if (!epoxy_has_egl_extension (NULL, "EGL_EXT_platform_base"))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Missing extension 'EGL_EXT_platform_base'");
      return FALSE;
    }

  egl_display = eglGetPlatformDisplayEXT (EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
  if (egl_display == EGL_NO_DISPLAY)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
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

  if (!epoxy_has_egl_extension (egl_display, "EGL_MESA_platform_surfaceless"))
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Missing extension 'EGL_MESA_platform_surfaceless'");
      return FALSE;
    }

  if (!epoxy_has_egl_extension (egl_display, "EGL_MESA_configless_context"))
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                   "Missing extension 'EGL_MESA_platform_surfaceless'");
      return FALSE;
    }

  egl_context = eglCreateContext (egl_display, EGL_NO_CONFIG_KHR, NULL, NULL);
  if (egl_context == EGL_NO_CONTEXT)
    {
      eglTerminate (egl_display);
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "Failed to create EGL context");
      return FALSE;
    }

  egl_thread->impl.egl_display = egl_display;
  egl_thread->impl.egl_context = egl_context;

  return TRUE;
}

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

  egl_thread->impl.main_loop = g_main_loop_new (egl_thread->impl.main_context,
                                                FALSE);
  g_mutex_lock (&egl_thread->mutex);
  egl_thread->impl.initialized = TRUE;
  g_cond_signal (&egl_thread->cond);
  g_mutex_unlock (&egl_thread->mutex);

  g_main_loop_run (egl_thread->impl.main_loop);

  g_clear_pointer (&egl_thread->impl.main_loop, g_main_loop_unref);

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
  g_mutex_init (&egl_thread->mutex);
  g_cond_init (&egl_thread->cond);

  g_mutex_lock (&egl_thread->mutex);

  egl_thread->thread = g_thread_new ("GRD EGL thread", grd_egl_thread_func, egl_thread);

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

  g_clear_pointer (&egl_thread->impl.main_context, g_main_context_unref);

  g_free (egl_thread);
}
