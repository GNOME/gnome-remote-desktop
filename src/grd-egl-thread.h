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

#ifndef GRD_EGL_THREAD_H
#define GRD_EGL_THREAD_H

#include <glib.h>
#include <stdint.h>

#include "grd-types.h"

typedef gpointer GrdEglThreadSlot;

typedef void (* GrdEglThreadCallback) (gboolean success,
                                       gpointer user_data);
typedef gboolean (* GrdEglThreadCustomFunc) (gpointer user_data);
typedef gboolean (* GrdEglThreadAllocBufferFunc) (gpointer user_data,
                                                  uint32_t pbo);
typedef void (* GrdEglThreadDeallocBufferFunc) (gpointer user_data);

GrdEglThread * grd_egl_thread_new (GError **error);

void grd_egl_thread_free (GrdEglThread *egl_thread);

const char * grd_egl_thread_get_drm_render_node (GrdEglThread *egl_thread);

void * grd_egl_thread_acquire_slot (GrdEglThread *egl_thread);

void grd_egl_thread_release_slot (GrdEglThread     *egl_thread,
                                  GrdEglThreadSlot  slot);

void grd_egl_thread_download (GrdEglThread         *egl_thread,
                              GrdEglThreadSlot      slot,
                              uint8_t              *dst_data,
                              int                   dst_row_width,
                              uint32_t              format,
                              unsigned int          width,
                              unsigned int          height,
                              uint32_t              n_planes,
                              const int            *fds,
                              const uint32_t       *strides,
                              const uint32_t       *offsets,
                              const uint64_t       *modifiers,
                              GrdEglThreadCallback  callback,
                              gpointer              user_data,
                              GDestroyNotify        destroy);

void grd_egl_thread_allocate (GrdEglThread                *egl_thread,
                              uint32_t                     height,
                              uint32_t                     stride,
                              GrdEglThreadAllocBufferFunc  allocate_func,
                              gpointer                     allocate_user_data,
                              GrdEglThreadCallback         callback,
                              gpointer                     user_data,
                              GDestroyNotify               destroy);

void grd_egl_thread_deallocate (GrdEglThread                  *egl_thread,
                                uint32_t                       pbo,
                                GrdEglThreadDeallocBufferFunc  deallocate_func,
                                gpointer                       deallocate_user_data,
                                GrdEglThreadCallback           callback,
                                gpointer                       user_data,
                                GDestroyNotify                 destroy);

void grd_egl_thread_upload (GrdEglThread                *egl_thread,
                            GrdEglThreadSlot             slot,
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
                            GDestroyNotify               destroy);

void grd_egl_thread_sync (GrdEglThread         *egl_thread,
                          GrdEglThreadCallback  callback,
                          gpointer              user_data,
                          GDestroyNotify        destroy);

void grd_egl_thread_run_custom_task (GrdEglThread           *egl_thread,
                                     GrdEglThreadCustomFunc  custom_func,
                                     gpointer                custom_func_data,
                                     GrdEglThreadCallback    callback,
                                     gpointer                user_data,
                                     GDestroyNotify          destroy);

gboolean grd_egl_thread_get_modifiers_for_format (GrdEglThread  *egl_thread,
                                                  uint32_t       format,
                                                  int           *out_n_modifiers,
                                                  uint64_t     **out_modifiers);

G_DEFINE_AUTOPTR_CLEANUP_FUNC (GrdEglThread, grd_egl_thread_free)

#endif /* GRD_EGL_THREAD_H */
