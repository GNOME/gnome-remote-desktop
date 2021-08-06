/*
 * Copyright (C) 2020-2021 Pascal Nowack
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
 */

#include "config.h"

#include "grd-rdp-fuse-clipboard.h"

#define FUSE_USE_VERSION 35

#include <fuse3/fuse_lowlevel.h>
#include <unistd.h>

#include "grd-clipboard-rdp.h"

#define CLIP_DATA_ENTRY_DROP_TIMEOUT_MS (60 * 1000)
#define CLIP_DATA_ENTRY_DROP_TIMEOUT_DELTA_US (10 * G_USEC_PER_SEC)
#define WIN32_FILETIME_TO_UNIX_EPOCH UINT64_C (11644473600)

typedef enum _FuseLowlevelOperationType
{
  FUSE_LL_OPERATION_NONE,
  FUSE_LL_OPERATION_LOOKUP,
  FUSE_LL_OPERATION_GETATTR,
  FUSE_LL_OPERATION_READ,
} FuseLowlevelOperationType;

typedef struct _FuseFileStealContext
{
  gboolean all_files;
  gboolean has_clip_data_id;
  uint32_t clip_data_id;

  GList *fuse_files;
} FuseFileStealContext;

typedef struct _ClearRdpFuseRequestContext
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard;
  gboolean all_files;
  gboolean has_clip_data_id;
  uint32_t clip_data_id;
} ClearRdpFuseRequestContext;

typedef struct _FuseFile FuseFile;
typedef struct _ClipDataEntry ClipDataEntry;

struct _FuseFile
{
  FuseFile *parent;
  GList *children;

  char *filename;
  char *filename_with_root;
  uint32_t list_idx;
  fuse_ino_t ino;

  gboolean is_directory;
  gboolean is_readonly;

  gboolean has_size;
  uint64_t size;

  gboolean has_last_write_time;
  uint64_t last_write_time_unix;

  gboolean has_clip_data_id;
  uint32_t clip_data_id;

  ClipDataEntry *entry;
};

struct _ClipDataEntry
{
  FuseFile *clip_data_dir;

  gboolean has_clip_data_id;
  uint32_t clip_data_id;

  GrdClipboardRdp *clipboard_rdp;

  gboolean had_file_contents_request;

  struct
  {
    GrdRdpFuseClipboard *rdp_fuse_clipboard;
    unsigned int drop_id;
    int64_t drop_id_timeout_set_us;
  } drop_context;
};

typedef struct _RdpFuseFileContentsRequest
{
  FuseFile *fuse_file;

  fuse_req_t fuse_req;
  FuseLowlevelOperationType operation_type;

  uint32_t stream_id;
} RdpFuseFileContentsRequest;

struct _GrdRdpFuseClipboard
{
  GObject parent;

  GrdClipboardRdp *clipboard_rdp;

  GThread *fuse_thread;
  struct fuse_session *fuse_handle;

  GSource *timeout_reset_source;
  GHashTable *timeouts_to_reset;

  GMutex filesystem_mutex;
  GMutex selection_mutex;
  GHashTable *inode_table;
  GHashTable *clip_data_table;
  GHashTable *request_table;
  FuseFile *root_dir;

  ClipDataEntry *no_cdi_entry;

  fuse_ino_t next_ino;
  uint32_t next_clip_data_id;
  uint32_t next_stream_id;
};

G_DEFINE_TYPE (GrdRdpFuseClipboard, grd_rdp_fuse_clipboard, G_TYPE_OBJECT);

static gboolean
should_remove_fuse_file (FuseFile *fuse_file,
                         gboolean  all_files,
                         gboolean  has_clip_data_id,
                         uint32_t  clip_data_id)
{
  if (all_files)
    return TRUE;

  if (fuse_file->ino == FUSE_ROOT_ID)
    return FALSE;
  if (!fuse_file->has_clip_data_id && !has_clip_data_id)
    return TRUE;
  if (fuse_file->has_clip_data_id && has_clip_data_id &&
      fuse_file->clip_data_id == clip_data_id)
    return TRUE;

  return FALSE;
}

static gboolean
collect_fuse_file_to_steal (gpointer key,
                            gpointer value,
                            gpointer user_data)
{
  FuseFile *fuse_file = value;
  FuseFileStealContext *steal_context = user_data;

  if (!should_remove_fuse_file (fuse_file,
                                steal_context->all_files,
                                steal_context->has_clip_data_id,
                                steal_context->clip_data_id))
    return FALSE;

  steal_context->fuse_files = g_list_prepend (steal_context->fuse_files,
                                              fuse_file);

  return TRUE;
}

static gboolean
maybe_clear_rdp_fuse_request (gpointer key,
                              gpointer value,
                              gpointer user_data)
{
  RdpFuseFileContentsRequest *rdp_fuse_request = value;
  ClearRdpFuseRequestContext *clear_context = user_data;
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clear_context->rdp_fuse_clipboard;

  if (!should_remove_fuse_file (rdp_fuse_request->fuse_file,
                                clear_context->all_files,
                                clear_context->has_clip_data_id,
                                clear_context->clip_data_id))
    return FALSE;

  if (rdp_fuse_clipboard->fuse_handle)
    fuse_reply_err (rdp_fuse_request->fuse_req, EIO);
  g_free (rdp_fuse_request);

  return TRUE;
}

void
grd_rdp_fuse_clipboard_dismiss_all_no_cdi_requests (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  ClearRdpFuseRequestContext clear_context = {0};

  clear_context.rdp_fuse_clipboard = rdp_fuse_clipboard;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  g_hash_table_foreach_remove (rdp_fuse_clipboard->request_table,
                               maybe_clear_rdp_fuse_request, &clear_context);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
}

static void
invalidate_inode (gpointer data,
                  gpointer user_data)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = user_data;
  FuseFile *fuse_file = data;
  FuseFile *child;
  GList *l;

  for (l = fuse_file->children; l; l = l->next)
    {
      child = l->data;

      fuse_lowlevel_notify_delete (rdp_fuse_clipboard->fuse_handle,
                                   fuse_file->ino, child->ino,
                                   child->filename, strlen (child->filename));
    }
  g_debug ("[FUSE Clipboard] Invalidating inode %lu for file \"%s\"",
           fuse_file->ino, fuse_file->filename);
  fuse_lowlevel_notify_inval_inode (rdp_fuse_clipboard->fuse_handle,
                                    fuse_file->ino, 0, 0);
  g_debug ("[FUSE Clipboard] Inode %lu invalidated", fuse_file->ino);
}

static void
fuse_file_free (gpointer data)
{
  FuseFile *fuse_file = data;

  g_list_free (fuse_file->children);
  g_free (fuse_file->filename_with_root);
  g_free (fuse_file);
}

static void
clear_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                 gboolean             all_selections,
                 ClipDataEntry       *entry)
{
  FuseFileStealContext steal_context = {0};
  ClearRdpFuseRequestContext clear_context = {0};
  FuseFile *clip_data_dir = NULL;

  g_assert (!g_mutex_trylock (&rdp_fuse_clipboard->selection_mutex));
  g_assert (!g_mutex_trylock (&rdp_fuse_clipboard->filesystem_mutex));

  if (entry)
    {
      FuseFile *root_dir = rdp_fuse_clipboard->root_dir;

      clip_data_dir = g_steal_pointer (&entry->clip_data_dir);
      root_dir->children = g_list_remove (root_dir->children, clip_data_dir);

      steal_context.has_clip_data_id = clear_context.has_clip_data_id =
        entry->has_clip_data_id;
      steal_context.clip_data_id = clear_context.clip_data_id =
        entry->clip_data_id;
    }
  steal_context.all_files = clear_context.all_files = all_selections;
  clear_context.rdp_fuse_clipboard = rdp_fuse_clipboard;

  if (entry && entry->has_clip_data_id)
    g_debug ("[FUSE Clipboard] Clearing selection for clipDataId %u", entry->clip_data_id);
  else
    g_debug ("[FUSE Clipboard] Clearing selection%s", all_selections ? "s" : "");
  g_hash_table_foreach_remove (rdp_fuse_clipboard->request_table,
                               maybe_clear_rdp_fuse_request, &clear_context);

  g_hash_table_foreach_steal (rdp_fuse_clipboard->inode_table,
                              collect_fuse_file_to_steal, &steal_context);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  /**
   * fuse_lowlevel_notify_inval_inode() is a blocking operation. If we receive
   * a FUSE request (e.g. read()), then FUSE would block in read() since
   * filesystem_mutex would still be locked, if we wouldn't unlock it here.
   * fuse_lowlevel_notify_inval_inode() will block, since it waits on the FUSE
   * operation to finish.
   * So, to avoid a deadlock here, unlock the mutex and reply all incoming
   * operations with -ENOENT until the invalidation process is complete.
   */
  g_list_foreach (steal_context.fuse_files, invalidate_inode, rdp_fuse_clipboard);
  if (clip_data_dir)
    {
      fuse_lowlevel_notify_delete (rdp_fuse_clipboard->fuse_handle,
                                   rdp_fuse_clipboard->root_dir->ino,
                                   clip_data_dir->ino, clip_data_dir->filename,
                                   strlen (clip_data_dir->filename));
    }
  g_list_free_full (steal_context.fuse_files, fuse_file_free);

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (entry && entry->has_clip_data_id)
    g_debug ("[FUSE Clipboard] Selection cleared for clipDataId %u", entry->clip_data_id);
  else
    g_debug ("[FUSE Clipboard] Selection%s cleared", all_selections ? "s" : "");
}

static void
clear_all_selections (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);

  clear_selection (rdp_fuse_clipboard, TRUE, NULL);

  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
}

static void
clear_entry_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                       ClipDataEntry       *entry)
{
  clear_selection (rdp_fuse_clipboard, FALSE, entry);
}

uint32_t
grd_rdp_fuse_clipboard_clip_data_id_new (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  GrdClipboardRdp *clipboard_rdp = rdp_fuse_clipboard->clipboard_rdp;
  ClipDataEntry *entry = NULL;

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);

  if (G_UNLIKELY (g_hash_table_size (rdp_fuse_clipboard->clip_data_table) >= UINT32_MAX))
    {
      GHashTableIter iter;
      ClipDataEntry *iter_value;

      g_debug ("[FUSE Clipboard] All clipDataIds used. Removing the oldest one");

      g_hash_table_iter_init (&iter, rdp_fuse_clipboard->clip_data_table);
      while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &iter_value))
        {
          g_assert (iter_value->drop_context.drop_id);
          if (!entry || entry->drop_context.drop_id_timeout_set_us >
                        iter_value->drop_context.drop_id_timeout_set_us)
            entry = iter_value;
        }

      g_debug ("[FUSE Clipboard] Force clearing selection with clipDataId %u",
               entry->clip_data_id);
      clear_entry_selection (rdp_fuse_clipboard, entry);

      g_hash_table_remove (rdp_fuse_clipboard->clip_data_table,
                           GUINT_TO_POINTER (entry->clip_data_id));
    }

  entry = g_malloc0 (sizeof (ClipDataEntry));
  entry->clipboard_rdp = clipboard_rdp;
  entry->has_clip_data_id = TRUE;

  entry->clip_data_id = rdp_fuse_clipboard->next_clip_data_id;
  while (g_hash_table_contains (rdp_fuse_clipboard->clip_data_table,
                                GUINT_TO_POINTER (entry->clip_data_id)))
    ++entry->clip_data_id;

  rdp_fuse_clipboard->next_clip_data_id = entry->clip_data_id + 1;

  g_hash_table_insert (rdp_fuse_clipboard->clip_data_table,
                       GUINT_TO_POINTER (entry->clip_data_id), entry);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);

  grd_clipboard_rdp_lock_remote_clipboard_data (clipboard_rdp, entry->clip_data_id);

  return entry->clip_data_id;
}

void
grd_rdp_fuse_clipboard_clip_data_id_free (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                          uint32_t             clip_data_id)
{
  ClipDataEntry *entry;

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!g_hash_table_lookup_extended (rdp_fuse_clipboard->clip_data_table,
                                     GUINT_TO_POINTER (clip_data_id),
                                     NULL, (gpointer *) &entry))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
      return;
    }
  clear_entry_selection (rdp_fuse_clipboard, entry);

  g_hash_table_remove (rdp_fuse_clipboard->clip_data_table,
                       GUINT_TO_POINTER (clip_data_id));
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
}

void
grd_rdp_fuse_clipboard_clear_no_cdi_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);

  if (rdp_fuse_clipboard->no_cdi_entry)
    clear_entry_selection (rdp_fuse_clipboard, rdp_fuse_clipboard->no_cdi_entry);

  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
}

static gboolean
drop_clip_data_entry (gpointer user_data)
{
  ClipDataEntry *entry = user_data;
  GrdRdpFuseClipboard *rdp_fuse_clipboard = entry->drop_context.rdp_fuse_clipboard;

  g_debug ("[FUSE Clipboard] Dropping ClipDataEntry with clipDataId %u",
           entry->clip_data_id);

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  clear_entry_selection (rdp_fuse_clipboard, entry);

  entry->drop_context.drop_id = 0;
  g_hash_table_remove (rdp_fuse_clipboard->clip_data_table,
                       GUINT_TO_POINTER (entry->clip_data_id));
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);

  return G_SOURCE_REMOVE;
}

static void
collect_instant_droppable_clip_data_entry (gpointer key,
                                           gpointer value,
                                           gpointer user_data)
{
  ClipDataEntry *entry = value;
  GList **droppable_clip_data_entries = user_data;

  if (!entry->had_file_contents_request)
    {
      *droppable_clip_data_entries = g_list_prepend (*droppable_clip_data_entries,
                                                     entry);
    }
}

static void
clear_instant_droppable_clip_data_entries (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  GList *droppable_clip_data_entries = NULL;
  GList *l;

  g_assert (!g_mutex_trylock (&rdp_fuse_clipboard->selection_mutex));
  g_assert (!g_mutex_trylock (&rdp_fuse_clipboard->filesystem_mutex));

  g_hash_table_foreach (rdp_fuse_clipboard->clip_data_table,
                        collect_instant_droppable_clip_data_entry,
                        &droppable_clip_data_entries);
  for (l = droppable_clip_data_entries; l; l = l->next)
    {
      ClipDataEntry *entry = l->data;

      g_debug ("[FUSE Clipboard] Instantly clearing selection for clipDataId %u",
               entry->clip_data_id);
      clear_entry_selection (rdp_fuse_clipboard, entry);

      g_hash_table_remove (rdp_fuse_clipboard->clip_data_table,
                           GUINT_TO_POINTER (entry->clip_data_id));
    }

  g_list_free (droppable_clip_data_entries);
}

static void
maybe_set_clip_data_entry_timeout (gpointer key,
                                   gpointer value,
                                   gpointer user_data)
{
  ClipDataEntry *entry = value;

  if (entry->drop_context.drop_id)
    return;

  g_debug ("[FUSE Clipboard] Setting timeout for selection with clipDataId %u",
           entry->clip_data_id);
  entry->drop_context.rdp_fuse_clipboard = user_data;
  entry->drop_context.drop_id = g_timeout_add (CLIP_DATA_ENTRY_DROP_TIMEOUT_MS,
                                               drop_clip_data_entry, entry);
  entry->drop_context.drop_id_timeout_set_us = g_get_monotonic_time ();
}

void
grd_rdp_fuse_clipboard_lazily_clear_all_cdi_selections (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  g_debug ("[FUSE Clipboard] Lazily clearing all selections with clipDataId");

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  clear_instant_droppable_clip_data_entries (rdp_fuse_clipboard);

  g_hash_table_foreach (rdp_fuse_clipboard->clip_data_table,
                        maybe_set_clip_data_entry_timeout,
                        rdp_fuse_clipboard);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
}

static fuse_ino_t
get_next_free_inode (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  fuse_ino_t ino = rdp_fuse_clipboard->next_ino;

  while (ino == 0 || ino == FUSE_ROOT_ID ||
         g_hash_table_contains (rdp_fuse_clipboard->inode_table,
                                GUINT_TO_POINTER (ino)))
    ++ino;

  rdp_fuse_clipboard->next_ino = ino + 1;

  return ino;
}

static FuseFile *
clip_data_dir_new (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                   gboolean             has_clip_data_id,
                   uint32_t             clip_data_id)
{
  FuseFile *root_dir = rdp_fuse_clipboard->root_dir;
  FuseFile *clip_data_dir;

  clip_data_dir = g_malloc0 (sizeof (FuseFile));
  clip_data_dir->parent = rdp_fuse_clipboard->root_dir;
  clip_data_dir->filename_with_root =
    has_clip_data_id ? g_strdup_printf ("/%u", clip_data_id)
                     : g_strdup_printf ("/%lu", GRD_RDP_FUSE_CLIPBOARD_NO_CLIP_DATA_ID);
  clip_data_dir->filename =
    strrchr (clip_data_dir->filename_with_root, '/') + 1;
  clip_data_dir->ino = get_next_free_inode (rdp_fuse_clipboard);
  clip_data_dir->is_directory = TRUE;
  clip_data_dir->is_readonly = TRUE;
  clip_data_dir->has_clip_data_id = has_clip_data_id;
  clip_data_dir->clip_data_id = clip_data_id;

  root_dir->children = g_list_append (root_dir->children, clip_data_dir);
  clip_data_dir->parent = root_dir;

  g_hash_table_insert (rdp_fuse_clipboard->inode_table,
                       GUINT_TO_POINTER (clip_data_dir->ino), clip_data_dir);

  return clip_data_dir;
}

static gboolean
is_fuse_file_parent (gpointer key,
                     gpointer value,
                     gpointer user_data)
{
  FuseFile *fuse_file = value;
  const char *parent_path = user_data;

  if (strcmp (parent_path, fuse_file->filename_with_root) == 0)
    return TRUE;

  return FALSE;
}

static FuseFile *
get_parent_directory (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                      const char          *path)
{
  FuseFile *parent;
  char *parent_path;

  parent_path = g_path_get_dirname (path);
  parent = g_hash_table_find (rdp_fuse_clipboard->inode_table,
                              is_fuse_file_parent, parent_path);

  g_free (parent_path);

  return parent;
}

static gboolean
set_selection_for_clip_data_entry (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                   FILEDESCRIPTORW     *files,
                                   uint32_t             n_files,
                                   ClipDataEntry       *entry)
{
  FuseFile *clip_data_dir = entry->clip_data_dir;
  uint32_t clip_data_id = clip_data_dir->clip_data_id;
  uint32_t i;

  if (entry->has_clip_data_id)
    g_debug ("[FUSE Clipboard] Setting selection for clipDataId %u", clip_data_id);
  else
    g_debug ("[FUSE Clipboard] Setting selection");

  for (i = 0; i < n_files; ++i)
    {
      FILEDESCRIPTORW *file;
      FuseFile *fuse_file, *parent;
      char *filename = NULL;
      uint32_t j;

      file = &files[i];

      fuse_file = g_malloc0 (sizeof (FuseFile));
      if (!(file->dwFlags & FD_ATTRIBUTES))
        g_warning ("[RDP.CLIPRDR] Client did not set the FD_ATTRIBUTES flag");

      if (ConvertFromUnicode (CP_UTF8, 0, file->cFileName, -1, &filename,
                              0, NULL, NULL) <= 0)
        {
          g_warning ("[RDP.CLIPRDR] Failed to convert filename. Aborting "
                     "SelectionTransfer");
          clear_entry_selection (rdp_fuse_clipboard, entry);

          g_free (fuse_file);

          return FALSE;
        }

      for (j = 0; filename[j]; ++j)
        {
          if (filename[j] == '\\')
            filename[j] = '/';
        }
      fuse_file->filename_with_root =
        g_strdup_printf ("%s/%s", clip_data_dir->filename_with_root, filename);
      fuse_file->filename = strrchr (fuse_file->filename_with_root, '/') + 1;
      g_free (filename);

      parent = get_parent_directory (rdp_fuse_clipboard,
                                     fuse_file->filename_with_root);
      parent->children = g_list_append (parent->children, fuse_file);
      fuse_file->parent = parent;

      fuse_file->list_idx = i;
      fuse_file->ino = get_next_free_inode (rdp_fuse_clipboard);
      fuse_file->has_clip_data_id = entry->has_clip_data_id;
      fuse_file->clip_data_id = clip_data_id;
      fuse_file->entry = entry;
      if (file->dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
        fuse_file->is_directory = TRUE;
      if (file->dwFileAttributes & FILE_ATTRIBUTE_READONLY)
        fuse_file->is_readonly = TRUE;
      if (file->dwFlags & FD_FILESIZE)
        {
          fuse_file->size = ((uint64_t) file->nFileSizeHigh << 32) +
                            file->nFileSizeLow;
          fuse_file->has_size = TRUE;
        }
      if (file->dwFlags & FD_WRITETIME)
        {
          uint64_t filetime;

          filetime = file->ftLastWriteTime.dwHighDateTime;
          filetime <<= 32;
          filetime += file->ftLastWriteTime.dwLowDateTime;

          fuse_file->last_write_time_unix = filetime / (10 * G_USEC_PER_SEC) -
                                            WIN32_FILETIME_TO_UNIX_EPOCH;
          fuse_file->has_last_write_time = TRUE;
        }
      g_hash_table_insert (rdp_fuse_clipboard->inode_table,
                           GUINT_TO_POINTER (fuse_file->ino), fuse_file);
    }
  if (entry->has_clip_data_id)
    g_debug ("[FUSE Clipboard] Selection set for clipDataId %u", clip_data_id);
  else
    g_debug ("[FUSE Clipboard] Selection set");

  return TRUE;
}

gboolean
grd_rdp_fuse_clipboard_set_cdi_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                          FILEDESCRIPTORW     *files,
                                          uint32_t             n_files,
                                          uint32_t             clip_data_id)
{
  ClipDataEntry *entry;
  gboolean result;

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!g_hash_table_lookup_extended (rdp_fuse_clipboard->clip_data_table,
                                     GUINT_TO_POINTER (clip_data_id),
                                     NULL, (gpointer *) &entry))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);
      return FALSE;
    }

  entry->clip_data_dir = clip_data_dir_new (rdp_fuse_clipboard,
                                            TRUE, clip_data_id);

  result = set_selection_for_clip_data_entry (rdp_fuse_clipboard,
                                              files, n_files, entry);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);

  return result;
}

gboolean
grd_rdp_fuse_clipboard_set_no_cdi_selection (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                             FILEDESCRIPTORW     *files,
                                             uint32_t             n_files)
{
  gboolean result;

  g_mutex_lock (&rdp_fuse_clipboard->selection_mutex);
  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!rdp_fuse_clipboard->no_cdi_entry)
    rdp_fuse_clipboard->no_cdi_entry = g_malloc0 (sizeof (ClipDataEntry));
  if (rdp_fuse_clipboard->no_cdi_entry->clip_data_dir)
    clear_entry_selection (rdp_fuse_clipboard, rdp_fuse_clipboard->no_cdi_entry);

  rdp_fuse_clipboard->no_cdi_entry->clip_data_dir =
    clip_data_dir_new (rdp_fuse_clipboard, FALSE, 0);

  result = set_selection_for_clip_data_entry (rdp_fuse_clipboard, files, n_files,
                                              rdp_fuse_clipboard->no_cdi_entry);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_unlock (&rdp_fuse_clipboard->selection_mutex);

  return result;
}

static void
write_file_attributes (FuseFile    *fuse_file,
                       struct stat *attr)
{
  memset (attr, 0, sizeof (struct stat));

  if (!fuse_file)
    return;

  attr->st_ino = fuse_file->ino;
  if (fuse_file->is_directory)
    {
      attr->st_mode = S_IFDIR | (fuse_file->is_readonly ? 0555 : 0755);
      attr->st_nlink = 2;
    }
  else
    {
      attr->st_mode = S_IFREG | (fuse_file->is_readonly ? 0444 : 0644);
      attr->st_nlink = 1;
      attr->st_size = fuse_file->size;
    }
  attr->st_uid = getuid ();
  attr->st_gid = getgid ();
  attr->st_atime = attr->st_mtime = attr->st_ctime =
    (fuse_file->has_last_write_time ? fuse_file->last_write_time_unix
                                    : time (NULL));
}

static void
maybe_queue_clip_data_entry_timeout_reset (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                           ClipDataEntry       *entry)
{
  int64_t drop_id_timeout_set_us = entry->drop_context.drop_id_timeout_set_us;
  int64_t now_us;

  if (!entry->drop_context.drop_id)
    return;

  now_us = g_get_monotonic_time ();
  if (now_us - drop_id_timeout_set_us < CLIP_DATA_ENTRY_DROP_TIMEOUT_DELTA_US)
    return;

  g_debug ("[FUSE Clipboard] Queueing a timeout reset for selection with "
           "clipDataId %u", entry->clip_data_id);
  g_hash_table_add (rdp_fuse_clipboard->timeouts_to_reset,
                    GUINT_TO_POINTER (entry->clip_data_id));
  g_source_set_ready_time (rdp_fuse_clipboard->timeout_reset_source, 0);
}

void
grd_rdp_fuse_clipboard_submit_file_contents_response (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                                      uint32_t             stream_id,
                                                      gboolean             response_ok,
                                                      const uint8_t       *data,
                                                      uint32_t             size)
{
  RdpFuseFileContentsRequest *rdp_fuse_request;
  struct fuse_entry_param entry = {0};

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!g_hash_table_steal_extended (rdp_fuse_clipboard->request_table,
                                    GUINT_TO_POINTER (stream_id),
                                    NULL, (gpointer *) &rdp_fuse_request))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      return;
    }

  if (!response_ok)
    {
      g_warning ("[RDP.CLIPRDR] Failed to retrieve file data for file \"%s\" "
                 "from the client", rdp_fuse_request->fuse_file->filename);
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

      fuse_reply_err (rdp_fuse_request->fuse_req, EIO);
      g_free (rdp_fuse_request);
      return;
    }

  maybe_queue_clip_data_entry_timeout_reset (rdp_fuse_clipboard,
                                             rdp_fuse_request->fuse_file->entry);

  if (rdp_fuse_request->operation_type == FUSE_LL_OPERATION_LOOKUP ||
      rdp_fuse_request->operation_type == FUSE_LL_OPERATION_GETATTR)
    {
      g_debug ("[FUSE Clipboard] Received file size for file \"%s\" with stream "
               "id %u", rdp_fuse_request->fuse_file->filename, stream_id);

      rdp_fuse_request->fuse_file->size = *((uint64_t *) data);
      rdp_fuse_request->fuse_file->has_size = TRUE;

      entry.ino = rdp_fuse_request->fuse_file->ino;
      write_file_attributes (rdp_fuse_request->fuse_file, &entry.attr);
      entry.attr_timeout = 1.0;
      entry.entry_timeout = 1.0;
    }
  else if (rdp_fuse_request->operation_type == FUSE_LL_OPERATION_READ)
    {
      g_debug ("[FUSE Clipboard] Received file range for file \"%s\" with stream "
               "id %u", rdp_fuse_request->fuse_file->filename, stream_id);
    }
  else
    {
      g_assert_not_reached ();
    }
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  switch (rdp_fuse_request->operation_type)
    {
    case FUSE_LL_OPERATION_NONE:
      break;
    case FUSE_LL_OPERATION_LOOKUP:
      fuse_reply_entry (rdp_fuse_request->fuse_req, &entry);
      break;
    case FUSE_LL_OPERATION_GETATTR:
      fuse_reply_attr (rdp_fuse_request->fuse_req, &entry.attr, entry.attr_timeout);
      break;
    case FUSE_LL_OPERATION_READ:
      fuse_reply_buf (rdp_fuse_request->fuse_req, (const char *) data, size);
      break;
    }

  g_free (rdp_fuse_request);
}

static RdpFuseFileContentsRequest *
rdp_fuse_file_contents_request_new (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                    FuseFile            *fuse_file,
                                    fuse_req_t           fuse_req)
{
  RdpFuseFileContentsRequest *rdp_fuse_request;
  uint32_t stream_id = rdp_fuse_clipboard->next_stream_id;

  fuse_file->entry->had_file_contents_request = TRUE;
  maybe_queue_clip_data_entry_timeout_reset (rdp_fuse_clipboard,
                                             fuse_file->entry);

  rdp_fuse_request = g_malloc0 (sizeof (RdpFuseFileContentsRequest));
  rdp_fuse_request->fuse_file = fuse_file;
  rdp_fuse_request->fuse_req = fuse_req;

  while (g_hash_table_contains (rdp_fuse_clipboard->request_table,
                                GUINT_TO_POINTER (stream_id)))
    ++stream_id;
  rdp_fuse_request->stream_id = stream_id;

  rdp_fuse_clipboard->next_stream_id = stream_id + 1;

  return rdp_fuse_request;
}

static void
request_file_size_async (GrdRdpFuseClipboard       *rdp_fuse_clipboard,
                         FuseFile                  *fuse_file,
                         fuse_req_t                 fuse_req,
                         FuseLowlevelOperationType  operation_type)
{
  GrdClipboardRdp *clipboard_rdp = rdp_fuse_clipboard->clipboard_rdp;
  RdpFuseFileContentsRequest *rdp_fuse_request;

  rdp_fuse_request = rdp_fuse_file_contents_request_new (rdp_fuse_clipboard,
                                                         fuse_file,
                                                         fuse_req);
  rdp_fuse_request->operation_type = operation_type;

  g_hash_table_insert (rdp_fuse_clipboard->request_table,
                       GUINT_TO_POINTER (rdp_fuse_request->stream_id),
                       rdp_fuse_request);

  g_debug ("[FUSE Clipboard] Requesting file size for file \"%s\" with stream id"
           " %u", fuse_file->filename, rdp_fuse_request->stream_id);
  grd_clipboard_rdp_request_remote_file_size_async (clipboard_rdp,
                                                    rdp_fuse_request->stream_id,
                                                    fuse_file->list_idx,
                                                    fuse_file->has_clip_data_id,
                                                    fuse_file->clip_data_id);
}

static void
request_file_range_async (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                          FuseFile            *fuse_file,
                          fuse_req_t           fuse_req,
                          uint64_t             offset,
                          uint32_t             requested_size)
{
  GrdClipboardRdp *clipboard_rdp = rdp_fuse_clipboard->clipboard_rdp;
  RdpFuseFileContentsRequest *rdp_fuse_request;

  rdp_fuse_request = rdp_fuse_file_contents_request_new (rdp_fuse_clipboard,
                                                         fuse_file,
                                                         fuse_req);
  rdp_fuse_request->operation_type = FUSE_LL_OPERATION_READ;

  g_hash_table_insert (rdp_fuse_clipboard->request_table,
                       GUINT_TO_POINTER (rdp_fuse_request->stream_id),
                       rdp_fuse_request);

  g_debug ("[FUSE Clipboard] Requesting file range (%u Bytes at offset %lu) for "
           "file \"%s\" with stream id %u", requested_size, offset,
           fuse_file->filename, rdp_fuse_request->stream_id);
  grd_clipboard_rdp_request_remote_file_range_async (clipboard_rdp,
                                                     rdp_fuse_request->stream_id,
                                                     fuse_file->list_idx,
                                                     offset, requested_size,
                                                     fuse_file->has_clip_data_id,
                                                     fuse_file->clip_data_id);
}

static FuseFile *
get_fuse_file_by_ino (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                      fuse_ino_t           fuse_ino)
{
  FuseFile *fuse_file;

  fuse_file = g_hash_table_lookup (rdp_fuse_clipboard->inode_table,
                                   GUINT_TO_POINTER (fuse_ino));

  return fuse_file;
}

static FuseFile *
get_fuse_file_by_name_from_parent (GrdRdpFuseClipboard *rdp_fuse_clipboard,
                                   FuseFile            *parent,
                                   const char          *name)
{
  FuseFile *child;
  GList *l;

  for (l = parent->children; l; l = l->next)
    {
      child = l->data;

      if (strcmp (name, child->filename) == 0)
        return child;
    }

  /**
   * This is not an error since several applications try to find specific files,
   * e.g. nautilus tries to find "/.Trash", etc.
   */
  g_debug ("[FUSE Clipboard] Requested file \"%s\" in directory \"%s\" does not "
           "exist", name, parent->filename);

  return NULL;
}

static void
fuse_ll_lookup (fuse_req_t  fuse_req,
                fuse_ino_t  parent_ino,
                const char *name)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *parent, *fuse_file;
  struct fuse_entry_param entry = {0};

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(parent = get_fuse_file_by_ino (rdp_fuse_clipboard, parent_ino)) ||
      !parent->is_directory)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }
  if (!(fuse_file = get_fuse_file_by_name_from_parent (
                      rdp_fuse_clipboard, parent, name)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }

  g_debug ("[FUSE Clipboard] lookup() has been called for \"%s\"", name);
  g_debug ("[FUSE Clipboard] Parent is \"%s\", child is \"%s\"",
           parent->filename_with_root, fuse_file->filename_with_root);

  if (!fuse_file->is_directory && !fuse_file->has_size)
    {
      request_file_size_async (rdp_fuse_clipboard, fuse_file, fuse_req,
                               FUSE_LL_OPERATION_LOOKUP);
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      return;
    }

  entry.ino = fuse_file->ino;
  write_file_attributes (fuse_file, &entry.attr);
  entry.attr_timeout = 1.0;
  entry.entry_timeout = 1.0;
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  fuse_reply_entry (fuse_req, &entry);
}

static void
fuse_ll_getattr (fuse_req_t             fuse_req,
                 fuse_ino_t             fuse_ino,
                 struct fuse_file_info *file_info)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *fuse_file;
  struct stat attr = {0};

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(fuse_file = get_fuse_file_by_ino (rdp_fuse_clipboard, fuse_ino)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }

  g_debug ("[FUSE Clipboard] getattr() has been called for file \"%s\"",
           fuse_file->filename_with_root);

  if (!fuse_file->is_directory && !fuse_file->has_size)
    {
      request_file_size_async (rdp_fuse_clipboard, fuse_file, fuse_req,
                               FUSE_LL_OPERATION_GETATTR);
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      return;
    }

  write_file_attributes (fuse_file, &attr);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  fuse_reply_attr (fuse_req, &attr, 1.0);
}

static void
fuse_ll_open (fuse_req_t             fuse_req,
              fuse_ino_t             fuse_ino,
              struct fuse_file_info *file_info)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *fuse_file;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(fuse_file = get_fuse_file_by_ino (rdp_fuse_clipboard, fuse_ino)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }
  if (fuse_file->is_directory)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, EISDIR);
      return;
    }
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  if ((file_info->flags & O_ACCMODE) != O_RDONLY)
    {
      fuse_reply_err (fuse_req, EACCES);
      return;
    }

  /* Using direct_io also increases FUSE_MAX_PAGES_PER_REQ */
  file_info->direct_io = 1;

  g_debug ("[FUSE Clipboard] Opening file \"%s\"", fuse_file->filename_with_root);
  fuse_reply_open (fuse_req, file_info);
}

static void
fuse_ll_read (fuse_req_t             fuse_req,
              fuse_ino_t             fuse_ino,
              size_t                 size,
              off_t                  offset,
              struct fuse_file_info *file_info)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *fuse_file;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(fuse_file = get_fuse_file_by_ino (rdp_fuse_clipboard, fuse_ino)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }
  if (fuse_file->is_directory)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, EISDIR);
      return;
    }
  if (!fuse_file->has_size || offset > fuse_file->size)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, EINVAL);
      return;
    }

  size = MIN (size, 8 * 1024 * 1024);
  g_assert (size > 0);

  request_file_range_async (rdp_fuse_clipboard, fuse_file, fuse_req,
                            offset, size);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
}

static void
fuse_ll_opendir (fuse_req_t             fuse_req,
                 fuse_ino_t             fuse_ino,
                 struct fuse_file_info *file_info)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *fuse_file;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(fuse_file = get_fuse_file_by_ino (rdp_fuse_clipboard, fuse_ino)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }
  if (!fuse_file->is_directory)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOTDIR);
      return;
    }
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  if ((file_info->flags & O_ACCMODE) != O_RDONLY)
    {
      fuse_reply_err (fuse_req, EACCES);
      return;
    }

  g_debug ("[FUSE Clipboard] Opening directory \"%s\"",
           fuse_file->filename_with_root);
  fuse_reply_open (fuse_req, file_info);
}

static void
fuse_ll_readdir (fuse_req_t             fuse_req,
                 fuse_ino_t             fuse_ino,
                 size_t                 max_size,
                 off_t                  offset,
                 struct fuse_file_info *file_info)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = fuse_req_userdata (fuse_req);
  FuseFile *fuse_file, *child;
  struct stat attr = {0};
  size_t written_size, entry_size;
  char *filename;
  char *buf;
  off_t i;
  GList *l;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  if (!(fuse_file = get_fuse_file_by_ino (rdp_fuse_clipboard, fuse_ino)))
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOENT);
      return;
    }
  if (!fuse_file->is_directory)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_err (fuse_req, ENOTDIR);
      return;
    }

  g_debug ("[FUSE Clipboard] Reading directory \"%s\" at offset %lu",
           fuse_file->filename_with_root, offset);

  if (offset >= g_list_length (fuse_file->children) + 1)
    {
      g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
      fuse_reply_buf (fuse_req, NULL, 0);
      return;
    }

  buf = g_malloc0 (max_size);
  written_size = 0;

  for (i = offset; i < 2; ++i)
    {
      if (i == 0)
        {
          write_file_attributes (fuse_file, &attr);
          filename = ".";
        }
      else if (i == 1)
        {
          write_file_attributes (fuse_file->parent, &attr);
          attr.st_ino = fuse_file->parent ? attr.st_ino : FUSE_ROOT_ID;
          attr.st_mode = fuse_file->parent ? attr.st_mode : 0555;
          filename = "..";
        }
      else
        {
          g_assert_not_reached ();
        }

      /**
       * buf needs to be large enough to hold the entry. If it's not, then the
       * entry is not filled in but the size of the entry is still returned.
       */
      entry_size = fuse_add_direntry (fuse_req, buf + written_size,
                                      max_size - written_size,
                                      filename, &attr, i);
      if (entry_size > max_size - written_size)
        break;

      written_size += entry_size;
    }

  for (l = fuse_file->children, i = 2; l; l = l->next, ++i)
    {
      if (i <= offset)
        continue;

      child = l->data;

      write_file_attributes (child, &attr);
      entry_size = fuse_add_direntry (fuse_req, buf + written_size,
                                      max_size - written_size,
                                      child->filename, &attr, i);
      if (entry_size > max_size - written_size)
        break;

      written_size += entry_size;
    }
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  fuse_reply_buf (fuse_req, buf, written_size);
  g_free (buf);
}

static const struct fuse_lowlevel_ops fuse_ll_ops =
{
  .lookup = fuse_ll_lookup,
  .getattr = fuse_ll_getattr,
  .open = fuse_ll_open,
  .read = fuse_ll_read,
  .opendir = fuse_ll_opendir,
  .readdir = fuse_ll_readdir,
};

static gpointer
fuse_thread_func (gpointer data)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = data;
  int result;

  g_debug ("[FUSE Clipboard] FUSE thread started");
  fuse_daemonize (1);

  result = fuse_session_loop (rdp_fuse_clipboard->fuse_handle);
  if (result < 0)
    g_error ("fuse_loop() failed: %s", g_strerror (-result));

  return NULL;
}

GrdRdpFuseClipboard *
grd_rdp_fuse_clipboard_new (GrdClipboardRdp *clipboard_rdp,
                            const char      *mount_path)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard;
  struct fuse_args args = {0};
  char *argv[1];

  argv[0] = program_invocation_name;
  args.argc = 1;
  args.argv = argv;

  rdp_fuse_clipboard = g_object_new (GRD_TYPE_RDP_FUSE_CLIPBOARD, NULL);
  rdp_fuse_clipboard->clipboard_rdp = clipboard_rdp;
  rdp_fuse_clipboard->fuse_handle = fuse_session_new (&args, &fuse_ll_ops,
                                                      sizeof (fuse_ll_ops),
                                                      rdp_fuse_clipboard);
  if (!rdp_fuse_clipboard->fuse_handle)
    g_error ("[FUSE Clipboard] Failed to create FUSE filesystem");

  if (fuse_session_mount (rdp_fuse_clipboard->fuse_handle, mount_path))
    g_error ("[FUSE Clipboard] Failed to mount FUSE filesystem");

  rdp_fuse_clipboard->fuse_thread = g_thread_new ("RDP FUSE clipboard thread",
                                                  fuse_thread_func,
                                                  rdp_fuse_clipboard);
  if (!rdp_fuse_clipboard->fuse_thread)
    g_error ("[FUSE Clipboard] Failed to create FUSE thread");

  return rdp_fuse_clipboard;
}

static void
dismiss_all_requests (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  ClearRdpFuseRequestContext clear_context = {0};

  clear_context.rdp_fuse_clipboard = rdp_fuse_clipboard;
  clear_context.all_files = TRUE;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  g_hash_table_foreach_remove (rdp_fuse_clipboard->request_table,
                               maybe_clear_rdp_fuse_request, &clear_context);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);
}

static void
grd_rdp_fuse_clipboard_dispose (GObject *object)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = GRD_RDP_FUSE_CLIPBOARD (object);

  if (rdp_fuse_clipboard->timeout_reset_source)
    g_source_destroy (rdp_fuse_clipboard->timeout_reset_source);
  g_clear_pointer (&rdp_fuse_clipboard->timeout_reset_source, g_source_unref);

  if (rdp_fuse_clipboard->fuse_handle)
    {
      clear_all_selections (rdp_fuse_clipboard);

      g_debug ("[FUSE Clipboard] Stopping FUSE thread");
      fuse_session_exit (rdp_fuse_clipboard->fuse_handle);
      dismiss_all_requests (rdp_fuse_clipboard);
      g_debug ("[FUSE Clipboard] Unmounting FUSE filesystem");
      fuse_session_unmount (rdp_fuse_clipboard->fuse_handle);

      g_debug ("[FUSE Clipboard] Waiting on FUSE thread");
      g_clear_pointer (&rdp_fuse_clipboard->fuse_thread, g_thread_join);
      g_debug ("[FUSE Clipboard] FUSE thread stopped");
      g_clear_pointer (&rdp_fuse_clipboard->fuse_handle, fuse_session_destroy);
    }

  if (rdp_fuse_clipboard->request_table)
    {
      dismiss_all_requests (rdp_fuse_clipboard);
      g_clear_pointer (&rdp_fuse_clipboard->request_table, g_hash_table_unref);
    }
  g_clear_pointer (&rdp_fuse_clipboard->no_cdi_entry, g_free);
  g_clear_pointer (&rdp_fuse_clipboard->clip_data_table, g_hash_table_destroy);
  g_clear_pointer (&rdp_fuse_clipboard->inode_table, g_hash_table_destroy);
  g_clear_pointer (&rdp_fuse_clipboard->timeouts_to_reset, g_hash_table_destroy);

  G_OBJECT_CLASS (grd_rdp_fuse_clipboard_parent_class)->dispose (object);
}

static void
clip_data_entry_free (gpointer data)
{
  ClipDataEntry *entry = data;

  grd_clipboard_rdp_unlock_remote_clipboard_data (entry->clipboard_rdp,
                                                  entry->clip_data_id);

  g_clear_handle_id (&entry->drop_context.drop_id, g_source_remove);

  g_free (entry);
}

static FuseFile *
fuse_file_new_root (void)
{
  FuseFile *root_dir;

  root_dir = g_malloc0 (sizeof (FuseFile));
  root_dir->filename_with_root = strdup ("/");
  root_dir->filename = root_dir->filename_with_root;
  root_dir->ino = FUSE_ROOT_ID;
  root_dir->is_directory = TRUE;
  root_dir->is_readonly = TRUE;

  return root_dir;
}

static gboolean
maybe_reset_clip_data_entry_timeout (gpointer key,
                                     gpointer value,
                                     gpointer user_data)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = user_data;
  uint32_t clip_data_id = GPOINTER_TO_UINT (key);
  ClipDataEntry *entry;

  if (!g_hash_table_lookup_extended (rdp_fuse_clipboard->clip_data_table,
                                     GUINT_TO_POINTER (clip_data_id),
                                     NULL, (gpointer *) &entry))
    return TRUE;

  if (!entry->drop_context.drop_id)
    return TRUE;

  g_debug ("[FUSE Clipboard] Resetting timeout for selection with clipDataId %u",
           clip_data_id);
  g_clear_handle_id (&entry->drop_context.drop_id, g_source_remove);
  entry->drop_context.drop_id = g_timeout_add (CLIP_DATA_ENTRY_DROP_TIMEOUT_MS,
                                               drop_clip_data_entry, entry);
  entry->drop_context.drop_id_timeout_set_us = g_get_monotonic_time ();

  return TRUE;
}

static gboolean
reset_clip_data_entry_timeouts (gpointer user_data)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = user_data;

  g_mutex_lock (&rdp_fuse_clipboard->filesystem_mutex);
  g_hash_table_foreach_remove (rdp_fuse_clipboard->timeouts_to_reset,
                               maybe_reset_clip_data_entry_timeout,
                               rdp_fuse_clipboard);
  g_mutex_unlock (&rdp_fuse_clipboard->filesystem_mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
timeout_reset_source_dispatch (GSource     *source,
                               GSourceFunc  callback,
                               gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs timeout_reset_source_funcs =
{
  .dispatch = timeout_reset_source_dispatch,
};

static void
grd_rdp_fuse_clipboard_init (GrdRdpFuseClipboard *rdp_fuse_clipboard)
{
  GSource *timeout_reset_source;

  rdp_fuse_clipboard->timeouts_to_reset = g_hash_table_new (NULL, NULL);
  rdp_fuse_clipboard->inode_table = g_hash_table_new (NULL, NULL);
  rdp_fuse_clipboard->clip_data_table = g_hash_table_new_full (NULL, NULL, NULL,
                                                               clip_data_entry_free);
  rdp_fuse_clipboard->request_table = g_hash_table_new (NULL, NULL);

  rdp_fuse_clipboard->root_dir = fuse_file_new_root ();
  g_hash_table_insert (rdp_fuse_clipboard->inode_table,
                       GUINT_TO_POINTER (rdp_fuse_clipboard->root_dir->ino),
                       rdp_fuse_clipboard->root_dir);

  g_mutex_init (&rdp_fuse_clipboard->filesystem_mutex);
  g_mutex_init (&rdp_fuse_clipboard->selection_mutex);

  timeout_reset_source = g_source_new (&timeout_reset_source_funcs,
                                       sizeof (GSource));
  g_source_set_callback (timeout_reset_source, reset_clip_data_entry_timeouts,
                         rdp_fuse_clipboard, NULL);
  g_source_set_ready_time (timeout_reset_source, -1);
  g_source_attach (timeout_reset_source, g_main_context_get_thread_default ());
  rdp_fuse_clipboard->timeout_reset_source = timeout_reset_source;
}

static void
grd_rdp_fuse_clipboard_class_init (GrdRdpFuseClipboardClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_fuse_clipboard_dispose;
}
