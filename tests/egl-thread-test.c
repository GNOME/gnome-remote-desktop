/*
 * Copyright (C) 2016, 2017, 2021 Red Hat Inc.
 * Copyright (C) 2018, 2019 DisplayLink (UK) Ltd.
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

#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <fcntl.h>
#include <gbm.h>
#include <gio/gio.h>
#include <gudev/gudev.h>
#include <math.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <xf86drm.h>

#include "grd-egl-thread.h"

#include <stdio.h>

#define DRM_CARD_UDEV_DEVICE_TYPE "drm_minor"

#define RED_FLOAT 0.5
#define GREEN_FLOAT 0.1
#define BLUE_FLOAT 0.8
#define ALPHA_FLOAT 1.0

#define FLOAT_ERROR_MARGIN 0.01

typedef struct
{
  uint32_t format;
  unsigned int width;
  unsigned int height;
  uint32_t n_planes;
  uint32_t strides[4];
  uint32_t offsets[4];
  uint64_t modifiers[4];
  int fds[4];

  uint8_t *read_back_data;
  int read_back_stride;
} DmaBuf;

static void
paint_image (int width,
             int height)
{
  glViewport (0, 0, width, width);
  glClearColor (RED_FLOAT, GREEN_FLOAT, BLUE_FLOAT, ALPHA_FLOAT);
  glClear (GL_COLOR_BUFFER_BIT);
  glFlush ();
  g_assert_cmpuint (glGetError (), ==, GL_NO_ERROR);
}

static EGLImageKHR
create_dmabuf_image (EGLDisplay  egl_display,
                     DmaBuf     *dma_buf)
{
  EGLint attribs[37];
  int atti = 0;
  EGLImageKHR egl_image;

  attribs[atti++] = EGL_WIDTH;
  attribs[atti++] = dma_buf->width;
  attribs[atti++] = EGL_HEIGHT;
  attribs[atti++] = dma_buf->height;
  attribs[atti++] = EGL_LINUX_DRM_FOURCC_EXT;
  attribs[atti++] = dma_buf->format;

  if (dma_buf->n_planes > 0)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE0_FD_EXT;
      attribs[atti++] = dma_buf->fds[0];
      attribs[atti++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
      attribs[atti++] = dma_buf->offsets[0];
      attribs[atti++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
      attribs[atti++] = dma_buf->strides[0];
      if (dma_buf->modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
          attribs[atti++] = dma_buf->modifiers[0] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
          attribs[atti++] = dma_buf->modifiers[0] >> 32;
        }
    }

  if (dma_buf->n_planes > 1)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE1_FD_EXT;
      attribs[atti++] = dma_buf->fds[1];
      attribs[atti++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
      attribs[atti++] = dma_buf->offsets[1];
      attribs[atti++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
      attribs[atti++] = dma_buf->strides[1];
      if (dma_buf->modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
          attribs[atti++] = dma_buf->modifiers[1] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
          attribs[atti++] = dma_buf->modifiers[1] >> 32;
        }
    }

  if (dma_buf->n_planes > 2)
    {
      attribs[atti++] = EGL_DMA_BUF_PLANE2_FD_EXT;
      attribs[atti++] = dma_buf->fds[2];
      attribs[atti++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
      attribs[atti++] = dma_buf->offsets[2];
      attribs[atti++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
      attribs[atti++] = dma_buf->strides[2];
      if (dma_buf->modifiers)
        {
          attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
          attribs[atti++] = dma_buf->modifiers[2] & 0xFFFFFFFF;
          attribs[atti++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
          attribs[atti++] = dma_buf->modifiers[2] >> 32;
        }
    }

  attribs[atti++] = EGL_NONE;
  g_assert (atti <= G_N_ELEMENTS (attribs));

  egl_image = eglCreateImageKHR (egl_display, EGL_NO_CONTEXT,
                                 EGL_LINUX_DMA_BUF_EXT, NULL,
                                 attribs);
  g_assert_cmpint (eglGetError (), ==, EGL_SUCCESS);
  return egl_image;
}

static void
bind_egl_image (EGLDisplay  egl_display,
                EGLImageKHR egl_image)
{
  GLuint tex;
  GLuint fbo;

  glGenTextures (1, &tex);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
  glTexParameteri (GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
  glBindTexture (GL_TEXTURE_2D, tex);
  glEGLImageTargetTexture2DOES (GL_TEXTURE_2D, egl_image);

  glGenFramebuffers (1, &fbo);
  glBindFramebuffer (GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D (GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                          GL_TEXTURE_2D, tex, 0);
  g_assert_cmpint (glCheckFramebufferStatus(GL_FRAMEBUFFER), ==,
                   GL_FRAMEBUFFER_COMPLETE);
  g_assert_cmpuint (glGetError (), ==, GL_NO_ERROR);
}

static DmaBuf *
dma_buf_new (void)
{
  DmaBuf *dma_buf;
  int i;

  dma_buf = g_new0 (DmaBuf, 1);
  for (i = 0; i < 4; i++)
    dma_buf->fds[i] = -1;

  return dma_buf;
}

static void
dma_buf_free (DmaBuf *dma_buf)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      int fd;

      fd = dma_buf->fds[i];
      if (fd != -1)
        close (fd);
    }

  g_free (dma_buf->read_back_data);
  g_free (dma_buf);
}

static DmaBuf *
generate_dma_buf (struct gbm_device *gbm_device,
                  EGLDisplay         egl_display,
                  int                width,
                  int                height)
{
  struct gbm_bo *bo;
  DmaBuf *dma_buf;
  int bo_fd;
  EGLImageKHR egl_image;
  int i;

  bo = gbm_bo_create (gbm_device,
                      width, height,
                      GBM_FORMAT_ARGB8888,
                      GBM_BO_USE_RENDERING);
  if (!bo)
    g_error ("Couldn't allocate a 10x10 GBM buffer: %s", g_strerror (errno));

  bo_fd = gbm_bo_get_fd (bo);
  g_assert_cmpint (bo_fd, >, 0);

  dma_buf = dma_buf_new ();

  dma_buf->format = gbm_bo_get_format (bo);
  dma_buf->width = gbm_bo_get_width (bo);
  dma_buf->height = gbm_bo_get_height (bo);

  dma_buf->n_planes = gbm_bo_get_plane_count (bo);
  for (i = 0; i < dma_buf->n_planes; i++)
    {
      dma_buf->strides[i] = gbm_bo_get_stride_for_plane (bo, i);
      dma_buf->offsets[i] = gbm_bo_get_offset (bo, i);
      dma_buf->modifiers[i] = gbm_bo_get_modifier (bo);
      dma_buf->fds[i] = bo_fd;
    }

  egl_image = create_dmabuf_image (egl_display, dma_buf);

  bind_egl_image (egl_display, egl_image);
  paint_image (dma_buf->width, dma_buf->height);

  eglDestroyImageKHR (egl_display, egl_image);
  gbm_bo_destroy (bo);

  return dma_buf;
}

static void
on_dma_buf_downloaded (gboolean success,
                       gpointer user_data)
{
  DmaBuf *dma_buf = user_data;
  int y;
  uint8_t *pixels = dma_buf->read_back_data;

  g_assert_true (success);

  for (y = 0; y < dma_buf->height; y++)
    {
      int x;

      for (x = 0; x < dma_buf->width; x++)
        {
          union {
            uint32_t argb32;
            uint8_t argb8888[4];
          } read_color = {
            .argb32 = ((uint32_t *) pixels)[x],
          };

          g_assert_cmpfloat_with_epsilon (read_color.argb8888[0] / 255.0,
                                          BLUE_FLOAT,
                                          FLOAT_ERROR_MARGIN);
          g_assert_cmpfloat_with_epsilon (read_color.argb8888[1] / 255.0,
                                          GREEN_FLOAT,
                                          FLOAT_ERROR_MARGIN);
          g_assert_cmpfloat_with_epsilon (read_color.argb8888[2] / 255.0,
                                          RED_FLOAT,
                                          FLOAT_ERROR_MARGIN);
          g_assert_cmpfloat_with_epsilon (read_color.argb8888[3] / 255.0,
                                          ALPHA_FLOAT,
                                          FLOAT_ERROR_MARGIN);
        }

      pixels += dma_buf->read_back_stride;
    }
}

static int
run_dma_buf_tests (struct gbm_device *gbm_device,
                   EGLDisplay         egl_display,
                   GrdEglThread      *egl_thread)
{
  DmaBuf *dma_buf;
  const int bpp = 4;
  const int width = 10;
  const int height = 15;
  const int dst_row_width = 20;

  dma_buf = generate_dma_buf (gbm_device, egl_display, width, height);

  dma_buf->read_back_stride = dst_row_width * bpp;
  dma_buf->read_back_data =
    g_malloc0 (dma_buf->height * dma_buf->read_back_stride);

  grd_egl_thread_download (egl_thread,
                           0, 0, 0,
                           NULL, NULL, NULL,
                           dma_buf->read_back_data,
                           dst_row_width,
                           dma_buf->format,
                           dma_buf->width,
                           dma_buf->height,
                           dma_buf->n_planes,
                           dma_buf->fds,
                           dma_buf->strides,
                           dma_buf->offsets,
                           dma_buf->modifiers,
                           on_dma_buf_downloaded,
                           dma_buf,
                           (GDestroyNotify) dma_buf_free);

  return EXIT_SUCCESS;
}

static struct gbm_device *
try_open_gbm_device (const char *path)
{
  int fd;
  struct gbm_device *gbm_device;

  fd = open (path, O_RDWR | O_CLOEXEC);
  if (fd == -1)
    return NULL;

  gbm_device = gbm_create_device (fd);
  if (!gbm_device)
    close (fd);

  return gbm_device;
}

static struct gbm_device *
open_gbm_device (void)
{
  const char *subsystems[] = { "drm", NULL };
  g_autoptr (GUdevClient) gudev_client = NULL;
  g_autoptr (GUdevEnumerator) enumerator = NULL;
  g_autolist (GUdevDevice) devices = NULL;
  GList *l;

  gudev_client = g_udev_client_new (subsystems);
  enumerator = g_udev_enumerator_new (gudev_client);

  g_udev_enumerator_add_match_name (enumerator, "card*");
  g_udev_enumerator_add_match_tag (enumerator, "seat");
  g_udev_enumerator_add_match_subsystem (enumerator, "drm");

  devices = g_udev_enumerator_execute (enumerator);

  for (l = devices; l; l = l->next)
    {
      GUdevDevice *device = l->data;
      const char *device_type;
      int fd;
      const char *path;
      g_autofree char *render_node_path = NULL;
      struct gbm_device *gbm_device;

      if (g_udev_device_get_device_type (device) != G_UDEV_DEVICE_TYPE_CHAR)
        continue;

      device_type = g_udev_device_get_property (device, "DEVTYPE");
      if (g_strcmp0 (device_type, DRM_CARD_UDEV_DEVICE_TYPE) != 0)
        continue;

      path = g_udev_device_get_device_file (device);
      fd = open (path, O_RDWR);
      if (fd == -1)
        {
          g_warning ("Failed to open '%s': %s", path, g_strerror (errno));
          continue;
        }

      render_node_path = drmGetRenderDeviceNameFromFd (fd);
      close (fd);

      gbm_device = try_open_gbm_device (render_node_path);
      if (gbm_device)
        return gbm_device;
    }

  return NULL;
}

int
main (int    argc,
      char **argv)
{
  GError *error = NULL;
  GrdEglThread *egl_thread;
  struct gbm_device *gbm_device;
  EGLDisplay egl_display;
  EGLContext egl_context;
  EGLint major, minor;
  EGLint *attrs;
  int retval = EXIT_SUCCESS;

  g_assert_cmpint (eglGetError (), ==, EGL_SUCCESS);

  egl_thread = grd_egl_thread_new (&error);
  if (!egl_thread)
    {
      if (g_error_matches (error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
        {
          g_message ("EGL not hardware accelerated, skipping test");
          retval = 77;
          goto out;
        }
      else
        {
          g_error ("Failed to start EGL thread: %s", error->message);
        }
    }

  g_assert_cmpint (eglGetError (), ==, EGL_SUCCESS);

  gbm_device = open_gbm_device ();
  if (!gbm_device)
    {
      g_warning ("No GBM device available, no way to test");
      retval = 77;
      goto out;
    }

  egl_display = eglGetPlatformDisplayEXT (EGL_PLATFORM_GBM_KHR,
                                          gbm_device,
                                          NULL);
  if (egl_display == EGL_NO_DISPLAY)
    {
      g_warning ("Couldn't get EGL gbm display");
      retval = 77;
      goto out;
    }

  if (!eglInitialize (egl_display, &major, &minor))
    g_error ("Failed to initialize EGL display");

  if (!eglBindAPI (EGL_OPENGL_ES_API))
    g_error ("Failed to bind GLES API");

  attrs = (EGLint[]) {
    EGL_CONTEXT_CLIENT_VERSION, 2,
    EGL_NONE
  };
  egl_context = eglCreateContext (egl_display, EGL_NO_CONFIG_KHR, EGL_NO_CONTEXT, attrs);

  eglMakeCurrent(egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, egl_context);

  retval = run_dma_buf_tests (gbm_device, egl_display, egl_thread);

  eglDestroyContext (egl_display, egl_context);
  eglTerminate (egl_display);

out:
  g_clear_pointer (&egl_thread, grd_egl_thread_free);

  return retval;
}
