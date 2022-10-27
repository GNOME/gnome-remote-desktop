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

#include "grd-clipboard-rdp.h"

#include <glib/gstdio.h>
#include <winpr/clipboard.h>

#include "grd-rdp-fuse-clipboard.h"
#include "grd-session-rdp.h"

#define CLIPRDR_FILEDESCRIPTOR_SIZE (4 + 32 + 4 + 16 + 8 + 4 + 4 + 520)
#define MAX_WAIT_TIME 4000

typedef struct _ServerFormatListUpdateContext
{
  GrdClipboardRdp *clipboard_rdp;
  GList *mime_type_tables;
} ServerFormatListUpdateContext;

typedef struct _ServerFormatDataRequestContext
{
  GrdClipboardRdp *clipboard_rdp;
  GrdMimeType mime_type;
  uint32_t src_format_id;
  uint32_t dst_format_id;
  gboolean needs_null_terminator;
  gboolean needs_conversion;
} ServerFormatDataRequestContext;

typedef struct _ClientFormatDataRequestContext
{
  GrdMimeTypeTable mime_type_table;

  gboolean has_clip_data_id;
  uint32_t clip_data_id;
} ClientFormatDataRequestContext;

typedef struct _ClipDataEntry
{
  uint32_t id;

  wClipboard *system;
  wClipboardDelegate *delegate;
  uint64_t serial;

  gboolean is_independent;
  gboolean has_file_list;
  gboolean requests_allowed;
} ClipDataEntry;

typedef struct _FormatData
{
  uint8_t *data;
  uint32_t size;
} FormatData;

struct _GrdClipboardRdp
{
  GrdClipboard parent;

  CliprdrServerContext *cliprdr_context;
  gboolean protocol_stopped;

  gboolean relieve_filename_restriction;

  wClipboard *system;
  wClipboardDelegate *delegate;
  gboolean has_file_list;
  uint64_t serial;

  GHashTable *allowed_server_formats;
  GList *pending_server_formats;
  GList *queued_server_formats;
  gboolean server_file_contents_requests_allowed;
  uint16_t format_list_response_msg_flags;

  GHashTable *serial_entry_table;
  GHashTable *clip_data_table;
  struct
  {
    ClipDataEntry *entry;
    ClipDataEntry *entry_to_replace;
    gboolean serial_already_in_use;
  } clipboard_retrieval_context;
  struct
  {
    ClipDataEntry *entry;
  } clipboard_destruction_context;

  char *fuse_mount_path;
  GrdRdpFuseClipboard *rdp_fuse_clipboard;
  GHashTable *format_data_cache;

  GrdMimeType which_unicode_format;
  ServerFormatDataRequestContext *format_data_request_context;

  GHashTable *pending_client_requests;
  GQueue *ordered_client_requests;

  GMutex client_request_mutex;
  ClientFormatDataRequestContext *current_client_request;
  CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response;
  unsigned int client_format_data_response_id;

  unsigned int pending_server_formats_drop_id;
  unsigned int client_request_abort_id;

  GCond completion_cond;
  GMutex completion_mutex;

  GMutex clip_data_entry_mutex;
  unsigned int clipboard_retrieval_id;
  unsigned int clipboard_destruction_id;
  gboolean completed_clip_data_entry;

  GMutex server_format_list_update_mutex;
  unsigned int server_format_list_update_id;
  gboolean completed_format_list;

  GMutex client_format_list_response_mutex;
  unsigned int client_format_list_response_id;

  GMutex server_format_data_request_mutex;
  unsigned int server_format_data_request_id;
  gboolean completed_format_data_request;
};

G_DEFINE_TYPE (GrdClipboardRdp, grd_clipboard_rdp, GRD_TYPE_CLIPBOARD)

static gboolean
send_mime_type_content_request (GrdClipboardRdp  *clipboard_rdp,
                                GrdMimeTypeTable *mime_type_table);

static void
create_new_winpr_clipboard (GrdClipboardRdp *clipboard_rdp);

static void
update_allowed_server_formats (GrdClipboardRdp *clipboard_rdp,
                               gboolean         received_response)
{
  GList *l;

  if (received_response)
    g_debug ("[RDP.CLIPRDR] Handling format list response from client");

  if (received_response &&
      clipboard_rdp->format_list_response_msg_flags & CB_RESPONSE_OK)
    {
      for (l = clipboard_rdp->pending_server_formats; l; l = l->next)
        {
          if (GPOINTER_TO_UINT (l->data) == GRD_MIME_TYPE_TEXT_URILIST)
            {
              ClipDataEntry *entry;

              if (g_hash_table_lookup_extended (clipboard_rdp->serial_entry_table,
                                                GUINT_TO_POINTER (clipboard_rdp->serial),
                                                NULL, (gpointer *) &entry))
                entry->requests_allowed = TRUE;

              clipboard_rdp->server_file_contents_requests_allowed = TRUE;
            }

          g_hash_table_add (clipboard_rdp->allowed_server_formats, l->data);
        }

      g_clear_pointer (&clipboard_rdp->pending_server_formats, g_list_free);

      return;
    }

  if (received_response &&
      !(clipboard_rdp->format_list_response_msg_flags & CB_RESPONSE_FAIL))
    {
      g_warning ("[RDP.CLIPRDR] Protocol violation: Client did not set response "
                 "flag. Assuming CB_RESPONSE_FAIL");
    }

  clipboard_rdp->server_file_contents_requests_allowed = FALSE;
  g_hash_table_remove_all (clipboard_rdp->allowed_server_formats);
  g_clear_pointer (&clipboard_rdp->pending_server_formats, g_list_free);
}

static void
remove_clipboard_format_data_for_mime_type (GrdClipboardRdp *clipboard_rdp,
                                            GrdMimeType      mime_type)
{
  FormatData *format_data;

  format_data = g_hash_table_lookup (clipboard_rdp->format_data_cache,
                                     GUINT_TO_POINTER (mime_type));
  if (!format_data)
    return;

  g_free (format_data->data);
  g_free (format_data);

  g_hash_table_remove (clipboard_rdp->format_data_cache,
                       GUINT_TO_POINTER (mime_type));
}

static void
remove_duplicated_clipboard_mime_types (GrdClipboardRdp  *clipboard_rdp,
                                        GList           **mime_type_list)
{
  GrdMimeType mime_type;

  /**
   * Remove the "x-special/gnome-copied-files" mimetype, since we use the
   * "text/uri-list" mimetype instead.
   */
  mime_type = GRD_MIME_TYPE_XS_GNOME_COPIED_FILES;
  if (g_list_find (*mime_type_list, GUINT_TO_POINTER (mime_type)))
    {
      *mime_type_list = g_list_remove (*mime_type_list, GUINT_TO_POINTER (mime_type));
      remove_clipboard_format_data_for_mime_type (clipboard_rdp, mime_type);
    }

  /**
   * We can only advertise CF_UNICODETEXT as remote format once, so ignore the
   * other local format if it exists.
   */
  if (g_list_find (*mime_type_list, GUINT_TO_POINTER (GRD_MIME_TYPE_TEXT_PLAIN_UTF8)) &&
      g_list_find (*mime_type_list, GUINT_TO_POINTER (GRD_MIME_TYPE_TEXT_UTF8_STRING)))
    {
      mime_type = GRD_MIME_TYPE_TEXT_PLAIN_UTF8;
      *mime_type_list = g_list_remove (*mime_type_list, GUINT_TO_POINTER (mime_type));
      remove_clipboard_format_data_for_mime_type (clipboard_rdp, mime_type);
    }
}

static void
update_clipboard_serial (GrdClipboardRdp *clipboard_rdp)
{
  ++clipboard_rdp->serial;
  while (g_hash_table_contains (clipboard_rdp->serial_entry_table,
                                GUINT_TO_POINTER (clipboard_rdp->serial)))
    ++clipboard_rdp->serial;

  g_debug ("[RDP.CLIPRDR] Updated clipboard serial to %lu", clipboard_rdp->serial);
}

static void
send_mime_type_list (GrdClipboardRdp *clipboard_rdp,
                     GList           *mime_type_list)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_LIST format_list = {0};
  CLIPRDR_FORMAT *cliprdr_formats;
  ClipDataEntry *entry;
  GrdMimeType mime_type;
  uint32_t n_formats;
  uint32_t i;
  GList *l;

  if (g_hash_table_lookup_extended (clipboard_rdp->serial_entry_table,
                                    GUINT_TO_POINTER (clipboard_rdp->serial),
                                    NULL, (gpointer *) &entry))
    {
      g_debug ("[RDP.CLIPRDR] ClipDataEntry with id %u and serial %lu is now "
               "independent", entry->id, entry->serial);
      entry->is_independent = TRUE;

      create_new_winpr_clipboard (clipboard_rdp);
    }
  update_clipboard_serial (clipboard_rdp);

  n_formats = g_list_length (mime_type_list);
  cliprdr_formats = g_malloc0 (n_formats * sizeof (CLIPRDR_FORMAT));
  for (i = 0, l = mime_type_list; i < n_formats; ++i, l = l->next)
    {
      mime_type = GPOINTER_TO_UINT (l->data);
      switch (mime_type)
        {
        case GRD_MIME_TYPE_TEXT_PLAIN:
          cliprdr_formats[i].formatId = CF_TEXT;
          break;
        case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
        case GRD_MIME_TYPE_TEXT_UTF8_STRING:
          cliprdr_formats[i].formatId = CF_UNICODETEXT;
          clipboard_rdp->which_unicode_format = mime_type;
          break;
        case GRD_MIME_TYPE_TEXT_HTML:
          cliprdr_formats[i].formatId = CB_FORMAT_HTML;
          cliprdr_formats[i].formatName = "HTML Format";
          break;
        case GRD_MIME_TYPE_IMAGE_BMP:
          cliprdr_formats[i].formatId = CF_DIB;
          break;
        case GRD_MIME_TYPE_IMAGE_TIFF:
          cliprdr_formats[i].formatId = CF_TIFF;
          break;
        case GRD_MIME_TYPE_IMAGE_GIF:
          cliprdr_formats[i].formatId = CB_FORMAT_GIF;
          break;
        case GRD_MIME_TYPE_IMAGE_JPEG:
          cliprdr_formats[i].formatId = CB_FORMAT_JPEG;
          break;
        case GRD_MIME_TYPE_IMAGE_PNG:
          cliprdr_formats[i].formatId = CB_FORMAT_PNG;
          break;
        case GRD_MIME_TYPE_TEXT_URILIST:
          /**
           * FileGroupDescriptorW does not have a consistent format id. It is
           * identified by its name.
           * When the client requests the content, it MUST use the id that we
           * told the client before when the clipboard format with the name
           * "FileGroupDescriptorW" was advertised.
           *
           * See also 1.3.1.2 Clipboard Format
           */
          cliprdr_formats[i].formatId = CB_FORMAT_TEXTURILIST;
          cliprdr_formats[i].formatName = "FileGroupDescriptorW";
          clipboard_rdp->server_file_contents_requests_allowed = FALSE;
          grd_rdp_fuse_clipboard_clear_no_cdi_selection (rdp_fuse_clipboard);
          grd_rdp_fuse_clipboard_lazily_clear_all_cdi_selections (rdp_fuse_clipboard);
          break;
        default:
          g_assert_not_reached ();
        }

      remove_clipboard_format_data_for_mime_type (clipboard_rdp, mime_type);
      g_hash_table_remove (clipboard_rdp->allowed_server_formats, l->data);

      clipboard_rdp->pending_server_formats =
        g_list_append (clipboard_rdp->pending_server_formats, l->data);
    }

  format_list.msgType = CB_FORMAT_LIST;
  format_list.formats = cliprdr_formats;
  format_list.numFormats = n_formats;

  g_debug ("[RDP.CLIPRDR] Sending FormatList");
  cliprdr_context->ServerFormatList (cliprdr_context, &format_list);

  g_free (cliprdr_formats);
  g_list_free (mime_type_list);
}

static gboolean
drop_pending_server_formats (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  GList *queued_server_formats;

  g_warning ("[RDP.CLIPRDR] Possible protocol violation: Client did not send "
             "format list response (Timeout reached)");
  update_allowed_server_formats (clipboard_rdp, FALSE);

  queued_server_formats = g_steal_pointer (&clipboard_rdp->queued_server_formats);
  if (queued_server_formats)
    send_mime_type_list (clipboard_rdp, queued_server_formats);

  clipboard_rdp->pending_server_formats_drop_id = 0;

  return G_SOURCE_REMOVE;
}

static void
grd_clipboard_rdp_update_client_mime_type_list (GrdClipboard *clipboard,
                                                GList        *mime_type_list)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (clipboard);

  remove_duplicated_clipboard_mime_types (clipboard_rdp, &mime_type_list);
  if (!mime_type_list)
    return;

  if (clipboard_rdp->pending_server_formats)
    {
      if (clipboard_rdp->queued_server_formats)
        g_debug ("[RDP.CLIPRDR] Replacing queued server FormatList");
      g_clear_pointer (&clipboard_rdp->queued_server_formats, g_list_free);

      g_debug ("[RDP.CLIPRDR] Queueing new FormatList");
      clipboard_rdp->queued_server_formats = mime_type_list;

      if (!clipboard_rdp->pending_server_formats_drop_id)
        {
          clipboard_rdp->pending_server_formats_drop_id =
            g_timeout_add (MAX_WAIT_TIME, drop_pending_server_formats, clipboard_rdp);
        }

      return;
    }

  send_mime_type_list (clipboard_rdp, mime_type_list);
}

void
grd_clipboard_rdp_lock_remote_clipboard_data (GrdClipboardRdp *clipboard_rdp,
                                              uint32_t         clip_data_id)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_LOCK_CLIPBOARD_DATA lock_clipboard_data = {0};

  g_debug ("[RDP.CLIPRDR] Locking clients clipboard data with clipDataId %u",
           clip_data_id);

  lock_clipboard_data.msgType = CB_LOCK_CLIPDATA;
  lock_clipboard_data.clipDataId = clip_data_id;

  cliprdr_context->ServerLockClipboardData (cliprdr_context,
                                            &lock_clipboard_data);
}

void
grd_clipboard_rdp_unlock_remote_clipboard_data (GrdClipboardRdp *clipboard_rdp,
                                                uint32_t         clip_data_id)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_UNLOCK_CLIPBOARD_DATA unlock_clipboard_data = {0};

  g_debug ("[RDP.CLIPRDR] Unlocking clients clipboard data associated to "
           "clipDataId %u", clip_data_id);

  unlock_clipboard_data.msgType = CB_UNLOCK_CLIPDATA;
  unlock_clipboard_data.clipDataId = clip_data_id;

  cliprdr_context->ServerUnlockClipboardData (cliprdr_context,
                                              &unlock_clipboard_data);
}

void
grd_clipboard_rdp_request_remote_file_size_async (GrdClipboardRdp *clipboard_rdp,
                                                  uint32_t         stream_id,
                                                  uint32_t         list_index,
                                                  gboolean         has_clip_data_id,
                                                  uint32_t         clip_data_id)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FILE_CONTENTS_REQUEST file_contents_request = {0};

  file_contents_request.msgType = CB_FILECONTENTS_REQUEST;
  file_contents_request.streamId = stream_id;
  file_contents_request.listIndex = list_index;
  file_contents_request.dwFlags = FILECONTENTS_SIZE;
  file_contents_request.cbRequested = 0x8;
  file_contents_request.haveClipDataId = has_clip_data_id;
  file_contents_request.clipDataId = clip_data_id;

  cliprdr_context->ServerFileContentsRequest (cliprdr_context,
                                              &file_contents_request);
}

void
grd_clipboard_rdp_request_remote_file_range_async (GrdClipboardRdp *clipboard_rdp,
                                                   uint32_t         stream_id,
                                                   uint32_t         list_index,
                                                   uint64_t         offset,
                                                   uint32_t         requested_size,
                                                   gboolean         has_clip_data_id,
                                                   uint32_t         clip_data_id)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FILE_CONTENTS_REQUEST file_contents_request = {0};

  file_contents_request.msgType = CB_FILECONTENTS_REQUEST;
  file_contents_request.streamId = stream_id;
  file_contents_request.listIndex = list_index;
  file_contents_request.dwFlags = FILECONTENTS_RANGE;
  file_contents_request.nPositionLow = offset & 0xFFFFFFFF;
  file_contents_request.nPositionHigh = offset >> 32 & 0xFFFFFFFF;
  file_contents_request.cbRequested = requested_size;
  file_contents_request.haveClipDataId = has_clip_data_id;
  file_contents_request.clipDataId = clip_data_id;

  cliprdr_context->ServerFileContentsRequest (cliprdr_context,
                                              &file_contents_request);
}

static void
track_serial_for_mime_type (GrdClipboardRdp *clipboard_rdp,
                            GrdMimeType      mime_type,
                            unsigned int     serial)
{
  GList *serials;

  serials = g_hash_table_lookup (clipboard_rdp->pending_client_requests,
                                 GUINT_TO_POINTER (mime_type));
  serials = g_list_append (serials, GUINT_TO_POINTER (serial));

  g_hash_table_insert (clipboard_rdp->pending_client_requests,
                       GUINT_TO_POINTER (mime_type), serials);
}

static void
enqueue_mime_type_content_request (GrdClipboardRdp  *clipboard_rdp,
                                   GrdMimeTypeTable *mime_type_table,
                                   unsigned int      serial)
{
  GrdMimeType mime_type = mime_type_table->mime_type;
  GrdMimeTypeTable *mime_type_table_copy;

  g_debug ("[RDP.CLIPRDR] Queueing mime type content request for mime type %s "
           "with serial %u", grd_mime_type_to_string (mime_type), serial);

  track_serial_for_mime_type (clipboard_rdp, mime_type, serial);

  mime_type_table_copy = g_malloc0 (sizeof (GrdMimeTypeTable));
  *mime_type_table_copy = *mime_type_table;
  g_queue_push_tail (clipboard_rdp->ordered_client_requests,
                     GUINT_TO_POINTER (mime_type_table_copy));
}

static void
abort_client_requests_for_serials (GrdClipboardRdp *clipboard_rdp,
                                   GList           *serials)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);
  unsigned int serial;
  GList *l;

  for (l = serials; l; l = l->next)
    {
      serial = GPOINTER_TO_UINT (l->data);
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         NULL, 0);
    }
  g_list_free (serials);
}

static void
abort_client_requests_for_mime_type (GrdClipboardRdp *clipboard_rdp,
                                     GrdMimeType      mime_type)
{
  GList *serials;

  if (!g_hash_table_steal_extended (clipboard_rdp->pending_client_requests,
                                    GUINT_TO_POINTER (mime_type),
                                    NULL, (gpointer *) &serials))
    return;

  abort_client_requests_for_serials (clipboard_rdp, serials);
}

static void
abort_client_requests_for_context (GrdClipboardRdp                *clipboard_rdp,
                                   ClientFormatDataRequestContext *request_context)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  GrdMimeTypeTable *mime_type_table = &request_context->mime_type_table;
  GrdMimeType mime_type = mime_type_table->mime_type;
  uint32_t clip_data_id = request_context->clip_data_id;

  g_debug ("[RDP.CLIPRDR] Aborting FormatDataRequest for mime type %s",
           grd_mime_type_to_string (mime_type));

  if (request_context->has_clip_data_id)
    grd_rdp_fuse_clipboard_clip_data_id_free (rdp_fuse_clipboard, clip_data_id);

  abort_client_requests_for_mime_type (clipboard_rdp, mime_type);
}

static void
maybe_send_next_mime_type_content_request (GrdClipboardRdp *clipboard_rdp)
{
  GrdMimeTypeTable *mime_type_table;
  GrdMimeType mime_type;
  GList *serials = NULL;

  mime_type_table = g_queue_pop_head (clipboard_rdp->ordered_client_requests);
  if (!mime_type_table)
    return;

  mime_type = mime_type_table->mime_type;
  serials = g_hash_table_lookup (clipboard_rdp->pending_client_requests,
                                 GUINT_TO_POINTER (mime_type));
  if (!serials)
    {
      g_free (mime_type_table);
      maybe_send_next_mime_type_content_request (clipboard_rdp);
      return;
    }

  if (!send_mime_type_content_request (clipboard_rdp, mime_type_table))
    {
      abort_client_requests_for_mime_type (clipboard_rdp, mime_type);
      g_free (mime_type_table);

      maybe_send_next_mime_type_content_request (clipboard_rdp);
      return;
    }

  g_free (mime_type_table);
}

static gboolean
abort_current_client_request (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  ClientFormatDataRequestContext *request_context;

  g_mutex_lock (&clipboard_rdp->client_request_mutex);
  request_context = g_steal_pointer (&clipboard_rdp->current_client_request);

  g_clear_handle_id (&clipboard_rdp->client_format_data_response_id,
                     g_source_remove);
  g_mutex_unlock (&clipboard_rdp->client_request_mutex);

  g_warning ("[RDP.CLIPRDR] Possible protocol violation: Client did not send "
             "format data response (Timeout reached)");
  abort_client_requests_for_context (clipboard_rdp, request_context);
  g_free (request_context);

  clipboard_rdp->client_request_abort_id = 0;
  maybe_send_next_mime_type_content_request (clipboard_rdp);

  return G_SOURCE_REMOVE;
}

static gboolean
send_mime_type_content_request (GrdClipboardRdp  *clipboard_rdp,
                                GrdMimeTypeTable *mime_type_table)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_DATA_REQUEST format_data_request = {0};
  GrdMimeType mime_type = mime_type_table->mime_type;
  ClientFormatDataRequestContext *request_context;
  uint32_t clip_data_id = 0;

  g_assert (mime_type != GRD_MIME_TYPE_NONE);
  g_assert (!clipboard_rdp->current_client_request);
  g_assert (!clipboard_rdp->client_format_data_response_id);
  g_assert (!clipboard_rdp->client_request_abort_id);

  request_context = g_malloc0 (sizeof (ClientFormatDataRequestContext));
  request_context->mime_type_table = *mime_type_table;

  if (clipboard_rdp->cliprdr_context->canLockClipData &&
      (mime_type == GRD_MIME_TYPE_TEXT_URILIST ||
       mime_type == GRD_MIME_TYPE_XS_GNOME_COPIED_FILES))
    {
      clip_data_id = grd_rdp_fuse_clipboard_clip_data_id_new (rdp_fuse_clipboard);
      request_context->clip_data_id = clip_data_id;
      request_context->has_clip_data_id = TRUE;
    }

  format_data_request.msgType = CB_FORMAT_DATA_REQUEST;
  format_data_request.dataLen = 4;
  format_data_request.requestedFormatId = mime_type_table->rdp.format_id;

  g_mutex_lock (&clipboard_rdp->client_request_mutex);
  if (cliprdr_context->ServerFormatDataRequest (cliprdr_context,
                                                &format_data_request))
    {
      g_mutex_unlock (&clipboard_rdp->client_request_mutex);

      if (request_context->has_clip_data_id)
        grd_rdp_fuse_clipboard_clip_data_id_free (rdp_fuse_clipboard, clip_data_id);

      g_free (request_context);

      return FALSE;
    }
  g_debug ("[RDP.CLIPRDR] Sent FormatDataRequest for mime type %s",
           grd_mime_type_to_string (mime_type));

  clipboard_rdp->current_client_request = request_context;
  g_mutex_unlock (&clipboard_rdp->client_request_mutex);

  clipboard_rdp->client_request_abort_id =
    g_timeout_add (MAX_WAIT_TIME, abort_current_client_request, clipboard_rdp);

  return TRUE;
}

static void
grd_clipboard_rdp_request_client_content_for_mime_type (GrdClipboard     *clipboard,
                                                        GrdMimeTypeTable *mime_type_table,
                                                        unsigned int      serial)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (clipboard);
  GrdMimeType mime_type = mime_type_table->mime_type;
  gboolean pending_format_list_update;
  FormatData *format_data;

  if (mime_type == GRD_MIME_TYPE_NONE)
    {
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         NULL, 0);
      return;
    }

  if (g_hash_table_lookup_extended (clipboard_rdp->format_data_cache,
                                    GUINT_TO_POINTER (mime_type),
                                    NULL, (gpointer *) &format_data))
    {
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         format_data->data,
                                                         format_data->size);
      return;
    }

  g_mutex_lock (&clipboard_rdp->server_format_list_update_mutex);
  pending_format_list_update = clipboard_rdp->server_format_list_update_id != 0;
  g_mutex_unlock (&clipboard_rdp->server_format_list_update_mutex);

  if (pending_format_list_update)
    {
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         NULL, 0);
      return;
    }

  if (clipboard_rdp->current_client_request)
    {
      enqueue_mime_type_content_request (clipboard_rdp, mime_type_table, serial);
      return;
    }

  g_assert (g_queue_is_empty (clipboard_rdp->ordered_client_requests));
  if (!send_mime_type_content_request (clipboard_rdp, mime_type_table))
    {
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         NULL, 0);
      return;
    }

  track_serial_for_mime_type (clipboard_rdp, mime_type, serial);
}

static void
serialize_file_list (FILEDESCRIPTORW  *files,
                     uint32_t          n_files,
                     uint8_t         **dst_data,
                     uint32_t         *dst_size)
{
  FILEDESCRIPTORW *file;
  wStream* s = NULL;
  uint64_t last_write_time;
  uint32_t i, j;

  if (!files || !dst_data || !dst_size)
    return;

  if (!(s = Stream_New (NULL, 4 + n_files * CLIPRDR_FILEDESCRIPTOR_SIZE)))
    return;

  Stream_Write_UINT32 (s, n_files);                    /* cItems */
  for (i = 0; i < n_files; ++i)
    {
      file = &files[i];

      Stream_Write_UINT32 (s, file->dwFlags);          /* flags */
      Stream_Zero (s, 32);                             /* reserved1 */
      Stream_Write_UINT32 (s, file->dwFileAttributes); /* fileAttributes */
      Stream_Zero (s, 16);                             /* reserved2 */

      last_write_time = file->ftLastWriteTime.dwHighDateTime;
      last_write_time <<= 32;
      last_write_time += file->ftLastWriteTime.dwLowDateTime;

      Stream_Write_UINT64 (s, last_write_time);        /* lastWriteTime */
      Stream_Write_UINT32 (s, file->nFileSizeHigh);    /* fileSizeHigh */
      Stream_Write_UINT32 (s, file->nFileSizeLow);     /* fileSizeLow */

      for (j = 0; j < 260; j++)                        /* cFileName */
        Stream_Write_UINT16 (s, file->cFileName[j]);
    }

  Stream_SealLength (s);
  Stream_GetLength (s, *dst_size);
  Stream_GetBuffer (s, *dst_data);

  Stream_Free (s, FALSE);
}

static void
grd_clipboard_rdp_submit_requested_server_content (GrdClipboard *clipboard,
                                                   uint8_t      *src_data,
                                                   uint32_t      src_size)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (clipboard);
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  ServerFormatDataRequestContext *request_context;
  CLIPRDR_FORMAT_DATA_RESPONSE format_data_response = {0};
  GrdMimeType mime_type;
  uint32_t src_format_id;
  uint32_t dst_format_id;
  uint8_t *dst_data = NULL;
  uint32_t dst_size = 0;
  BOOL success;

  request_context = g_steal_pointer (&clipboard_rdp->format_data_request_context);
  mime_type = request_context->mime_type;
  src_format_id = request_context->src_format_id;
  dst_format_id = request_context->dst_format_id;

  if (src_data)
    {
      if (request_context->needs_conversion)
        {
          if (request_context->needs_null_terminator)
            {
              char *pnull_terminator;

              src_data = g_realloc (src_data, src_size + 1);
              pnull_terminator = (char *) src_data + src_size;
              *pnull_terminator = '\0';
              ++src_size;
            }

          success = ClipboardSetData (clipboard_rdp->system,
                                      src_format_id, src_data, src_size);
          if (success)
            {
              dst_data = ClipboardGetData (clipboard_rdp->system,
                                           dst_format_id, &dst_size);

              if (dst_data && mime_type == GRD_MIME_TYPE_TEXT_URILIST)
                {
                  uint64_t serial = clipboard_rdp->serial;
                  ClipDataEntry *entry;
                  FILEDESCRIPTORW *files;
                  uint32_t n_files;

                  files = (FILEDESCRIPTORW *) dst_data;
                  n_files = dst_size / sizeof (FILEDESCRIPTORW);

                  dst_data = NULL;
                  dst_size = 0;
                  serialize_file_list (files, n_files, &dst_data, &dst_size);

                  g_free (files);

                  clipboard_rdp->has_file_list = TRUE;
                  if (g_hash_table_lookup_extended (clipboard_rdp->serial_entry_table,
                                                    GUINT_TO_POINTER (serial),
                                                    NULL, (gpointer *) &entry))
                    entry->has_file_list = TRUE;
                }
            }
          if (!success || !dst_data)
            {
              g_warning ("[RDP.CLIPRDR] Converting clipboard content for "
                         "client failed");
            }
        }
      else
        {
          dst_data = g_steal_pointer (&src_data);
          dst_size = src_size;
        }
    }

  format_data_response.msgType = CB_FORMAT_DATA_RESPONSE;
  format_data_response.msgFlags = dst_data ? CB_RESPONSE_OK : CB_RESPONSE_FAIL;
  format_data_response.dataLen = dst_size;
  format_data_response.requestedFormatData = dst_data;

  cliprdr_context->ServerFormatDataResponse (cliprdr_context,
                                             &format_data_response);

  g_free (src_data);
  g_free (dst_data);
  g_free (request_context);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  clipboard_rdp->completed_format_data_request = TRUE;
  g_cond_signal (&clipboard_rdp->completion_cond);
  g_mutex_unlock (&clipboard_rdp->completion_mutex);
}

/**
 * FreeRDP already updated our capabilites after the client told us
 * about its capabilities, there is nothing to do here
 */
static uint32_t
cliprdr_client_capabilities (CliprdrServerContext       *cliprdr_context,
                             const CLIPRDR_CAPABILITIES *capabilities)
{
  g_autoptr (GStrvBuilder) client_capabilities = NULL;
  char **client_caps_strings;
  g_autofree char *caps_string = NULL;

  client_capabilities = g_strv_builder_new ();

  if (cliprdr_context->useLongFormatNames)
    g_strv_builder_add (client_capabilities, "long format names");
  if (cliprdr_context->streamFileClipEnabled)
    g_strv_builder_add (client_capabilities, "stream file clip");
  if (cliprdr_context->fileClipNoFilePaths)
    g_strv_builder_add (client_capabilities, "file clip no file paths");
  if (cliprdr_context->canLockClipData)
    g_strv_builder_add (client_capabilities, "can lock clip data");
  if (cliprdr_context->hasHugeFileSupport)
    g_strv_builder_add (client_capabilities, "huge file support");

  client_caps_strings = g_strv_builder_end (client_capabilities);
  caps_string = g_strjoinv (", ", client_caps_strings);
  g_message ("[RDP.CLIPRDR] Client capabilities: %s", caps_string);

  g_strfreev (client_caps_strings);

  return CHANNEL_RC_OK;
}

/**
 * Client sent us a Temporary Directory PDU.
 * We don't handle the CF_HDROP format however. It's a relict of the past.
 */
static uint32_t
cliprdr_temp_directory (CliprdrServerContext         *cliprdr_context,
                        const CLIPRDR_TEMP_DIRECTORY *temp_directory)
{
  g_debug ("[RDP.CLIPRDR] Client sent a Temporary Directory PDU with path \"%s\"",
           temp_directory->szTempDir);

  return CHANNEL_RC_OK;
}

static void
abort_all_client_requests (GrdClipboardRdp *clipboard_rdp)
{
  ClientFormatDataRequestContext *request_context;
  GHashTableIter iter;
  GList *serials;

  g_debug ("[RDP.CLIPRDR] Aborting all pending client requests");

  g_mutex_lock (&clipboard_rdp->client_request_mutex);
  request_context = g_steal_pointer (&clipboard_rdp->current_client_request);

  g_clear_handle_id (&clipboard_rdp->client_format_data_response_id,
                     g_source_remove);
  g_mutex_unlock (&clipboard_rdp->client_request_mutex);

  if (request_context)
    abort_client_requests_for_context (clipboard_rdp, request_context);

  g_free (request_context);
  g_clear_handle_id (&clipboard_rdp->client_request_abort_id, g_source_remove);

  g_hash_table_iter_init (&iter, clipboard_rdp->pending_client_requests);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &serials))
    {
      abort_client_requests_for_serials (clipboard_rdp, serials);
      g_hash_table_iter_remove (&iter);
    }

  g_queue_clear_full (clipboard_rdp->ordered_client_requests, g_free);
}

static gboolean
update_server_format_list (gpointer user_data)
{
  ServerFormatListUpdateContext *update_context = user_data;
  GrdClipboardRdp *clipboard_rdp = update_context->clipboard_rdp;
  GrdClipboard *clipboard = GRD_CLIPBOARD (update_context->clipboard_rdp);
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_LIST_RESPONSE format_list_response = {0};
  GList *mime_type_tables;
  GrdMimeTypeTable *mime_type_table;
  GList *l;

  mime_type_tables = g_steal_pointer (&update_context->mime_type_tables);
  for (l = mime_type_tables; l; l = l->next)
    {
      mime_type_table = l->data;

      /* Also indirectly handles the GRD_MIME_TYPE_XS_GNOME_COPIED_FILES case */
      if (mime_type_table->mime_type == GRD_MIME_TYPE_TEXT_URILIST)
        {
          clipboard_rdp->server_file_contents_requests_allowed = FALSE;

          grd_rdp_fuse_clipboard_clear_no_cdi_selection (rdp_fuse_clipboard);
          grd_rdp_fuse_clipboard_lazily_clear_all_cdi_selections (rdp_fuse_clipboard);
        }

      remove_clipboard_format_data_for_mime_type (clipboard_rdp,
                                                  mime_type_table->mime_type);
      g_hash_table_remove (clipboard_rdp->allowed_server_formats,
                           GUINT_TO_POINTER (mime_type_table->mime_type));
    }

  abort_all_client_requests (clipboard_rdp);
  grd_clipboard_update_server_mime_type_list (clipboard, mime_type_tables);

  format_list_response.msgType = CB_FORMAT_LIST_RESPONSE;
  format_list_response.msgFlags = CB_RESPONSE_OK;

  cliprdr_context->ServerFormatListResponse (cliprdr_context,
                                             &format_list_response);

  /**
   * Any FileContentsRequest that is still waiting for a FileContentsResponse
   * won't get a response any more, when the client does not support clipboard
   * data locking. So, drop all requests without clipDataId here.
   */
  if (!cliprdr_context->canLockClipData)
    grd_rdp_fuse_clipboard_dismiss_all_no_cdi_requests (rdp_fuse_clipboard);

  g_mutex_lock (&clipboard_rdp->server_format_list_update_mutex);
  clipboard_rdp->server_format_list_update_id = 0;
  g_mutex_unlock (&clipboard_rdp->server_format_list_update_mutex);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  clipboard_rdp->completed_format_list = TRUE;
  g_cond_signal (&clipboard_rdp->completion_cond);
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  return G_SOURCE_REMOVE;
}

static void
update_context_free (gpointer data)
{
  ServerFormatListUpdateContext *update_context = data;

  g_clear_list (&update_context->mime_type_tables, g_free);

  g_free (update_context);
}

/**
 * Client notifies us that its clipboard is updated with new clipboard data
 */
static uint32_t
cliprdr_client_format_list (CliprdrServerContext      *cliprdr_context,
                            const CLIPRDR_FORMAT_LIST *format_list)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  ServerFormatListUpdateContext *update_context;
  GHashTable *found_mime_types;
  GrdMimeTypeTable *mime_type_table = NULL;
  GList *mime_type_tables = NULL;
  GrdMimeType mime_type;
  gboolean already_has_text_format = FALSE;
  uint32_t i;

  found_mime_types = g_hash_table_new (NULL, NULL);

  /**
   * The text format CF_TEXT can depend on the CF_LOCALE content. In such
   * situations, we might not get the correct content from WinPR.
   * CF_UNICODETEXT is not affected by the CF_LOCALE content, so try to find
   * the CF_UNICODETEXT first to ensure that we use CF_UNICODETEXT, when both
   * formats are available and CF_TEXT is listed first.
   */
  for (i = 0; i < format_list->numFormats; ++i)
    {
      if (format_list->formats[i].formatId == CF_UNICODETEXT)
        {
          mime_type = GRD_MIME_TYPE_TEXT_PLAIN_UTF8;

          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

          g_hash_table_add (found_mime_types, GUINT_TO_POINTER (mime_type));
          mime_type = GRD_MIME_TYPE_TEXT_UTF8_STRING;

          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

          g_hash_table_add (found_mime_types, GUINT_TO_POINTER (mime_type));
          already_has_text_format = TRUE;
          g_debug ("[RDP.CLIPRDR] Force using CF_UNICODETEXT over CF_TEXT as "
                   "external format for text/plain;charset=utf-8 and UTF8_STRING");
          break;
        }
    }

  for (i = 0; i < format_list->numFormats; ++i)
    {
      mime_type = GRD_MIME_TYPE_NONE;

      /**
       * FileGroupDescriptorW does not have a consistent id. The name however,
       * is always the same.
       *
       * If the client uses short format names, the formatName is truncated to
       * either 32 ASCII 8 characters or 16 UTF-16 characters.
       */
      if (format_list->formats[i].formatName &&
          (strcmp (format_list->formats[i].formatName, "FileGroupDescriptorW") == 0 ||
           strcmp (format_list->formats[i].formatName, "FileGroupDescri") == 0))
        {
          if (!g_hash_table_contains (found_mime_types,
                                      GUINT_TO_POINTER (GRD_MIME_TYPE_TEXT_URILIST)))
            {
              /**
               * Advertise the "x-special/gnome-copied-files" format in addition to
               * the "text/uri-list" format
               */
              mime_type = GRD_MIME_TYPE_XS_GNOME_COPIED_FILES;

              mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
              mime_type_table->mime_type = mime_type;
              mime_type_table->rdp.format_id = format_list->formats[i].formatId;
              mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

              g_hash_table_add (found_mime_types, GUINT_TO_POINTER (mime_type));
            }

          mime_type = GRD_MIME_TYPE_TEXT_URILIST;
        }
      else if (format_list->formats[i].formatName &&
               strcmp (format_list->formats[i].formatName, "HTML Format") == 0)
        {
          mime_type = GRD_MIME_TYPE_TEXT_HTML;
        }
      else
        {
          switch (format_list->formats[i].formatId)
            {
            case CF_TEXT:
            case CF_UNICODETEXT:
            case CF_OEMTEXT:
              if (!already_has_text_format)
                {
                  mime_type = GRD_MIME_TYPE_TEXT_PLAIN_UTF8;

                  g_assert (!g_hash_table_contains (found_mime_types,
                                                    GUINT_TO_POINTER (mime_type)));

                  mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
                  mime_type_table->mime_type = mime_type;
                  mime_type_table->rdp.format_id = format_list->formats[i].formatId;
                  mime_type_tables = g_list_append (mime_type_tables,
                                                    mime_type_table);
                  g_hash_table_add (found_mime_types, GUINT_TO_POINTER (mime_type));

                  mime_type = GRD_MIME_TYPE_TEXT_UTF8_STRING;
                  already_has_text_format = TRUE;
                  g_debug ("[RDP.CLIPRDR] Client advertised data for "
                           "text/plain;charset=utf-8 and UTF8_STRING "
                           "(external format: id: %u, name: %s)",
                           format_list->formats[i].formatId,
                           format_list->formats[i].formatName);
                }
              break;
            case CF_DIB:
              mime_type = GRD_MIME_TYPE_IMAGE_BMP;
              break;
            case CF_TIFF:
              mime_type = GRD_MIME_TYPE_IMAGE_TIFF;
              break;
            case CB_FORMAT_GIF:
              mime_type = GRD_MIME_TYPE_IMAGE_GIF;
              break;
            case CB_FORMAT_JPEG:
              mime_type = GRD_MIME_TYPE_IMAGE_JPEG;
              break;
            case CB_FORMAT_PNG:
              mime_type = GRD_MIME_TYPE_IMAGE_PNG;
              break;
            default:
              g_debug ("[RDP.CLIPRDR] Client advertised unknown format: id: %u, "
                       "name: %s", format_list->formats[i].formatId,
                       format_list->formats[i].formatName);
            }
        }

      if (mime_type != GRD_MIME_TYPE_NONE &&
          !g_hash_table_contains (found_mime_types, GUINT_TO_POINTER (mime_type)))
        {
          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

          g_hash_table_add (found_mime_types, GUINT_TO_POINTER (mime_type));
        }
      else if (mime_type != GRD_MIME_TYPE_NONE)
        {
          g_debug ("[RDP.CLIPRDR] Ignoring duplicated format: id: %u, name: %s",
                   format_list->formats[i].formatId,
                   format_list->formats[i].formatName);
        }
    }

  g_hash_table_destroy (found_mime_types);

  if (clipboard_rdp->server_format_list_update_id)
    {
      g_debug ("[RDP.CLIPRDR] Wrong message sequence: Got new format list "
               "without being able to response to last update first");
    }

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  while (!clipboard_rdp->protocol_stopped &&
         !clipboard_rdp->completed_format_list)
    {
      g_cond_wait (&clipboard_rdp->completion_cond,
                   &clipboard_rdp->completion_mutex);
    }
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  if (clipboard_rdp->protocol_stopped)
    {
      g_list_free_full (mime_type_tables, g_free);
      return CHANNEL_RC_OK;
    }

  g_assert (clipboard_rdp->completed_format_list);
  clipboard_rdp->completed_format_list = FALSE;

  update_context = g_malloc0 (sizeof (ServerFormatListUpdateContext));
  update_context->clipboard_rdp = clipboard_rdp;
  update_context->mime_type_tables = mime_type_tables;

  g_mutex_lock (&clipboard_rdp->server_format_list_update_mutex);
  clipboard_rdp->server_format_list_update_id =
    g_idle_add_full (G_PRIORITY_DEFAULT_IDLE, update_server_format_list,
                     update_context, update_context_free);
  g_mutex_unlock (&clipboard_rdp->server_format_list_update_mutex);

  return CHANNEL_RC_OK;
}

static gboolean
handle_format_list_response (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  GList *queued_server_formats;

  if (clipboard_rdp->pending_server_formats)
    update_allowed_server_formats (clipboard_rdp, TRUE);

  g_clear_handle_id (&clipboard_rdp->pending_server_formats_drop_id,
                     g_source_remove);

  g_mutex_lock (&clipboard_rdp->client_format_list_response_mutex);
  clipboard_rdp->client_format_list_response_id = 0;
  g_mutex_unlock (&clipboard_rdp->client_format_list_response_mutex);

  queued_server_formats = g_steal_pointer (&clipboard_rdp->queued_server_formats);
  if (queued_server_formats)
    send_mime_type_list (clipboard_rdp, queued_server_formats);

  return G_SOURCE_REMOVE;
}

/**
 * Client sent us a response to our format list pdu
 */
static uint32_t
cliprdr_client_format_list_response (CliprdrServerContext               *cliprdr_context,
                                     const CLIPRDR_FORMAT_LIST_RESPONSE *format_list_response)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  g_autoptr (GMutexLocker) locker = NULL;

  locker = g_mutex_locker_new (&clipboard_rdp->client_format_list_response_mutex);
  if (clipboard_rdp->client_format_list_response_id != 0)
    {
      g_warning ("[RDP.CLIPRDR] Wrong message sequence: Received an unexpected "
                 "format list response. Ignoring...");
      return CHANNEL_RC_OK;
    }

  clipboard_rdp->format_list_response_msg_flags = format_list_response->msgFlags;

  clipboard_rdp->client_format_list_response_id =
    g_idle_add (handle_format_list_response, clipboard_rdp);

  return CHANNEL_RC_OK;
}

static gboolean
retrieve_current_clipboard (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  ClipDataEntry *entry = clipboard_rdp->clipboard_retrieval_context.entry;
  ClipDataEntry *entry_to_replace;
  uint64_t serial = clipboard_rdp->serial;
  gboolean serial_already_in_use;

  g_debug ("[RDP.CLIPRDR] Tracking serial %lu for ClipDataEntry",
           clipboard_rdp->serial);

  entry_to_replace = clipboard_rdp->clipboard_retrieval_context.entry_to_replace;
  if (entry_to_replace)
    {
      g_hash_table_remove (clipboard_rdp->serial_entry_table,
                           GUINT_TO_POINTER (entry_to_replace->serial));
    }

  serial_already_in_use = g_hash_table_contains (clipboard_rdp->serial_entry_table,
                                                 GUINT_TO_POINTER (serial));
  if (!serial_already_in_use)
    {
      g_hash_table_insert (clipboard_rdp->serial_entry_table,
                           GUINT_TO_POINTER (serial), entry);
    }
  clipboard_rdp->clipboard_retrieval_context.serial_already_in_use =
    serial_already_in_use;

  entry->serial = serial;
  entry->has_file_list = clipboard_rdp->has_file_list;
  entry->requests_allowed = clipboard_rdp->server_file_contents_requests_allowed;

  entry->system = clipboard_rdp->system;
  entry->delegate = clipboard_rdp->delegate;

  g_mutex_lock (&clipboard_rdp->clip_data_entry_mutex);
  clipboard_rdp->clipboard_retrieval_id = 0;
  g_mutex_unlock (&clipboard_rdp->clip_data_entry_mutex);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  clipboard_rdp->completed_clip_data_entry = TRUE;
  g_cond_signal (&clipboard_rdp->completion_cond);
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  return G_SOURCE_REMOVE;
}

static gboolean
is_clip_data_entry_user_of_serial (gpointer key,
                                   gpointer value,
                                   gpointer user_data)
{
  ClipDataEntry *entry = value;
  uint64_t serial = GPOINTER_TO_UINT (user_data);

  return entry->serial == serial;
}

/**
 * Client requests us to retain all file stream data on the clipboard
 */
static uint32_t
cliprdr_client_lock_clipboard_data (CliprdrServerContext              *cliprdr_context,
                                    const CLIPRDR_LOCK_CLIPBOARD_DATA *lock_clipboard_data)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  uint32_t clip_data_id = lock_clipboard_data->clipDataId;
  g_autofree ClipDataEntry *entry = NULL;

  if (clipboard_rdp->protocol_stopped)
    return CHANNEL_RC_OK;

  g_debug ("[RDP.CLIPRDR] Client requested a lock with clipDataId: %u",
           clip_data_id);

  clipboard_rdp->clipboard_retrieval_context.entry_to_replace = NULL;
  if (g_hash_table_lookup_extended (clipboard_rdp->clip_data_table,
                                    GUINT_TO_POINTER (clip_data_id),
                                    NULL, (gpointer *) &entry))
    {
      g_warning ("[RDP.CLIPRDR] Protocol violation: Client requested a lock with"
                 " an existing clipDataId %u. Replacing existing ClipDataEntry",
                 clip_data_id);

      clipboard_rdp->clipboard_retrieval_context.entry_to_replace = entry;
    }

  g_assert (clipboard_rdp->completed_clip_data_entry);
  clipboard_rdp->completed_clip_data_entry = FALSE;

  entry = g_malloc0 (sizeof (ClipDataEntry));
  clipboard_rdp->clipboard_retrieval_context.entry = entry;

  g_mutex_lock (&clipboard_rdp->clip_data_entry_mutex);
  clipboard_rdp->clipboard_retrieval_id =
    g_idle_add (retrieve_current_clipboard, clipboard_rdp);
  g_mutex_unlock (&clipboard_rdp->clip_data_entry_mutex);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  while (!clipboard_rdp->protocol_stopped &&
         !clipboard_rdp->completed_clip_data_entry)
    {
      g_cond_wait (&clipboard_rdp->completion_cond,
                   &clipboard_rdp->completion_mutex);
    }
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  if (clipboard_rdp->protocol_stopped)
    return CHANNEL_RC_OK;

  g_assert (clipboard_rdp->completed_clip_data_entry);

  if (clipboard_rdp->clipboard_retrieval_context.serial_already_in_use)
    {
      uint64_t serial = entry->serial;

      g_warning ("[RDP.CLIPRDR] Protocol violation: Double lock detected ("
                 "Clipboard serial already in use). Replacing the clipDataId.");

      g_free (entry);
      entry = g_hash_table_find (clipboard_rdp->clip_data_table,
                                 is_clip_data_entry_user_of_serial,
                                 GUINT_TO_POINTER (serial));
      g_hash_table_steal (clipboard_rdp->clip_data_table,
                          GUINT_TO_POINTER (entry->id));
    }

  entry->id = clip_data_id;

  g_debug ("[RDP.CLIPRDR] Tracking lock with clipDataId %u", clip_data_id);
  g_hash_table_insert (clipboard_rdp->clip_data_table,
                       GUINT_TO_POINTER (clip_data_id),
                       g_steal_pointer (&entry));

  return CHANNEL_RC_OK;
}

static gboolean
handle_clip_data_entry_destruction (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  ClipDataEntry *entry;

  entry = g_steal_pointer (&clipboard_rdp->clipboard_destruction_context.entry);

  g_debug ("[RDP.CLIPRDR] Deleting ClipDataEntry with serial %lu", entry->serial);
  g_hash_table_remove (clipboard_rdp->serial_entry_table,
                       GUINT_TO_POINTER (entry->serial));

  g_mutex_lock (&clipboard_rdp->clip_data_entry_mutex);
  clipboard_rdp->clipboard_destruction_id = 0;
  g_mutex_unlock (&clipboard_rdp->clip_data_entry_mutex);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  clipboard_rdp->completed_clip_data_entry = TRUE;
  g_cond_signal (&clipboard_rdp->completion_cond);
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  return G_SOURCE_REMOVE;
}

/**
 * Client notifies us that the file stream data for a specific clip data id MUST
 * now be released
 */
static uint32_t
cliprdr_client_unlock_clipboard_data (CliprdrServerContext                *cliprdr_context,
                                      const CLIPRDR_UNLOCK_CLIPBOARD_DATA *unlock_clipboard_data)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  uint32_t clip_data_id = unlock_clipboard_data->clipDataId;
  ClipDataEntry *entry;

  if (clipboard_rdp->protocol_stopped)
    return CHANNEL_RC_OK;

  g_debug ("[RDP.CLIPRDR] Client requested an unlock with clipDataId: %u",
           clip_data_id);
  if (!g_hash_table_lookup_extended (clipboard_rdp->clip_data_table,
                                     GUINT_TO_POINTER (clip_data_id),
                                     NULL, (gpointer *) &entry))
    {
      g_warning ("[RDP.CLIPRDR] Protocol violation: ClipDataEntry with id %u "
                 "does not exist", clip_data_id);
      return CHANNEL_RC_OK;
    }

  g_debug ("[RDP.CLIPRDR] Removing lock with clipDataId: %u", clip_data_id);

  g_assert (clipboard_rdp->completed_clip_data_entry);
  clipboard_rdp->completed_clip_data_entry = FALSE;

  clipboard_rdp->clipboard_destruction_context.entry = entry;

  g_mutex_lock (&clipboard_rdp->clip_data_entry_mutex);
  clipboard_rdp->clipboard_destruction_id =
    g_idle_add (handle_clip_data_entry_destruction, clipboard_rdp);
  g_mutex_unlock (&clipboard_rdp->clip_data_entry_mutex);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  while (!clipboard_rdp->protocol_stopped &&
         !clipboard_rdp->completed_clip_data_entry)
    {
      g_cond_wait (&clipboard_rdp->completion_cond,
                   &clipboard_rdp->completion_mutex);
    }
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  g_debug ("[RDP.CLIPRDR] Untracking lock with clipDataId %u", clip_data_id);
  g_hash_table_remove (clipboard_rdp->clip_data_table,
                       GUINT_TO_POINTER (clip_data_id));

  return CHANNEL_RC_OK;
}

static gboolean
request_server_format_data (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  ServerFormatDataRequestContext *request_context;
  GrdMimeType mime_type;

  request_context = clipboard_rdp->format_data_request_context;
  mime_type = request_context->mime_type;

  if (!g_hash_table_contains (clipboard_rdp->allowed_server_formats,
                              GUINT_TO_POINTER (mime_type)))
    {
      CLIPRDR_FORMAT_DATA_RESPONSE format_data_response = {0};

      format_data_response.msgType = CB_FORMAT_DATA_RESPONSE;
      format_data_response.msgFlags = CB_RESPONSE_FAIL;

      cliprdr_context->ServerFormatDataResponse (cliprdr_context,
                                                 &format_data_response);

      g_clear_pointer (&clipboard_rdp->format_data_request_context, g_free);

      g_mutex_lock (&clipboard_rdp->server_format_data_request_mutex);
      clipboard_rdp->server_format_data_request_id = 0;
      g_mutex_unlock (&clipboard_rdp->server_format_data_request_mutex);

      g_mutex_lock (&clipboard_rdp->completion_mutex);
      clipboard_rdp->completed_format_data_request = TRUE;
      g_cond_signal (&clipboard_rdp->completion_cond);
      g_mutex_unlock (&clipboard_rdp->completion_mutex);

      return G_SOURCE_REMOVE;
    }

  grd_clipboard_request_server_content_for_mime_type_async (clipboard,
                                                            mime_type);

  g_mutex_lock (&clipboard_rdp->server_format_data_request_mutex);
  clipboard_rdp->server_format_data_request_id = 0;
  g_mutex_unlock (&clipboard_rdp->server_format_data_request_mutex);

  return G_SOURCE_REMOVE;
}

/**
 * Client knows our format list, it requests now the data from our clipboard
 */
static uint32_t
cliprdr_client_format_data_request (CliprdrServerContext              *cliprdr_context,
                                    const CLIPRDR_FORMAT_DATA_REQUEST *format_data_request)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  ServerFormatDataRequestContext *request_context;
  CLIPRDR_FORMAT_DATA_RESPONSE format_data_response = {0};
  GrdMimeType mime_type = GRD_MIME_TYPE_NONE;
  uint32_t src_format_id = 0;
  uint32_t dst_format_id;
  gboolean needs_null_terminator = FALSE;
  gboolean needs_conversion = FALSE;

  dst_format_id = format_data_request->requestedFormatId;
  switch (dst_format_id)
    {
    case CF_TEXT:
      mime_type = GRD_MIME_TYPE_TEXT_PLAIN;
      needs_null_terminator = TRUE;
      needs_conversion = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "text/plain");
      break;
    case CF_UNICODETEXT:
      mime_type = clipboard_rdp->which_unicode_format;
      needs_null_terminator = TRUE;
      needs_conversion = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "UTF8_STRING");
      break;
    case CB_FORMAT_HTML:
      mime_type = GRD_MIME_TYPE_TEXT_HTML;
      needs_null_terminator = TRUE;
      needs_conversion = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "text/html");
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "HTML Format");
      break;
    case CF_DIB:
      mime_type = GRD_MIME_TYPE_IMAGE_BMP;
      needs_conversion = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "image/bmp");
      break;
    case CF_TIFF:
      mime_type = GRD_MIME_TYPE_IMAGE_TIFF;
      break;
    case CB_FORMAT_GIF:
      mime_type = GRD_MIME_TYPE_IMAGE_GIF;
      break;
    case CB_FORMAT_JPEG:
      mime_type = GRD_MIME_TYPE_IMAGE_JPEG;
      break;
    case CB_FORMAT_PNG:
      mime_type = GRD_MIME_TYPE_IMAGE_PNG;
      break;
    case CB_FORMAT_TEXTURILIST:
      mime_type = GRD_MIME_TYPE_TEXT_URILIST;
      needs_conversion = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "text/uri-list");
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "FileGroupDescriptorW");
      break;
    }

  if (clipboard_rdp->server_format_data_request_id)
    {
      g_debug ("[RDP.CLIPRDR] Wrong message sequence: Got new format data "
               "request without being able to push response for last request "
               "yet.");
    }

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  while (!clipboard_rdp->protocol_stopped &&
         !clipboard_rdp->completed_format_data_request)
    {
      g_cond_wait (&clipboard_rdp->completion_cond,
                   &clipboard_rdp->completion_mutex);
    }
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  if (clipboard_rdp->protocol_stopped)
    return CHANNEL_RC_OK;

  if (mime_type != GRD_MIME_TYPE_NONE)
    {
      g_assert (clipboard_rdp->completed_format_data_request);
      clipboard_rdp->completed_format_data_request = FALSE;

      request_context = g_malloc0 (sizeof (ServerFormatDataRequestContext));
      request_context->clipboard_rdp = clipboard_rdp;
      request_context->mime_type = mime_type;
      request_context->src_format_id = src_format_id;
      request_context->dst_format_id = dst_format_id;
      request_context->needs_null_terminator = needs_null_terminator;
      request_context->needs_conversion = needs_conversion;

      g_assert (!clipboard_rdp->format_data_request_context);
      clipboard_rdp->format_data_request_context = request_context;

      g_mutex_lock (&clipboard_rdp->server_format_data_request_mutex);
      clipboard_rdp->server_format_data_request_id =
        g_idle_add (request_server_format_data, clipboard_rdp);
      g_mutex_unlock (&clipboard_rdp->server_format_data_request_mutex);

      return CHANNEL_RC_OK;
    }

  format_data_response.msgType = CB_FORMAT_DATA_RESPONSE;
  format_data_response.msgFlags = CB_RESPONSE_FAIL;
  format_data_response.dataLen = 0;
  format_data_response.requestedFormatData = NULL;

  return cliprdr_context->ServerFormatDataResponse (cliprdr_context,
                                                    &format_data_response);
}

static gboolean
extract_format_data_response (GrdClipboardRdp               *clipboard_rdp,
                              CLIPRDR_FORMAT_DATA_RESPONSE  *format_data_response,
                              uint8_t                      **data,
                              uint32_t                      *size)
{
  gboolean response_ok;

  *data = (uint8_t *) format_data_response->requestedFormatData;
  *size = format_data_response->dataLen;

  response_ok = format_data_response->msgFlags & CB_RESPONSE_OK;
  *size = response_ok && *data ? *size : 0;

  if (!response_ok)
    g_clear_pointer (data, g_free);

  return response_ok && *data;
}

static gboolean
filedescriptorw_filename_is_valid (const WCHAR *filename)
{
  uint16_t i;

  if (filename[0] == L'\0')
    return FALSE;

  for (i = 1; i < 260; ++i)
    {
      if (filename[i] == L'\0')
        return TRUE;
    }

  return FALSE;
}

static uint8_t *
get_uri_list_from_packet_file_list (GrdClipboardRdp *clipboard_rdp,
                                    uint8_t         *src_data,
                                    uint32_t         src_size,
                                    uint32_t        *dst_size,
                                    gboolean         has_clip_data_id,
                                    uint32_t         clip_data_id)
{
  g_autofree FILEDESCRIPTORW *files = NULL;
  FILEDESCRIPTORW *file;
  uint32_t n_files = 0;
  g_autofree char *clip_data_dir_name = NULL;
  char *filename = NULL;
  char *escaped_name;
  char *file_uri;
  GArray *dst_data;
  uint32_t i;

  clip_data_dir_name = has_clip_data_id ?
    g_strdup_printf ("%u", clip_data_id) :
    g_strdup_printf ("%lu", GRD_RDP_FUSE_CLIPBOARD_NO_CLIP_DATA_ID);

  *dst_size = 0;
  dst_data = g_array_new (TRUE, TRUE, sizeof (char));

  cliprdr_parse_file_list (src_data, src_size, &files, &n_files);
  for (i = 0; i < n_files; ++i)
    {
      file = &files[i];

      if (!filedescriptorw_filename_is_valid (file->cFileName))
        {
          g_array_free (dst_data, TRUE);
          return NULL;
        }
      if (ConvertFromUnicode (CP_UTF8, 0, file->cFileName, -1, &filename,
                              0, NULL, NULL) <= 0)
        {
          g_array_free (dst_data, TRUE);
          return NULL;
        }
      if (strchr (filename, '\\') || strchr (filename, '/'))
        {
          g_free (filename);
          continue;
        }

      escaped_name = g_uri_escape_string (filename,
                                          G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
                                          TRUE);
      file_uri = g_strdup_printf ("file://%s/%s/%s\r\n",
                                  clipboard_rdp->fuse_mount_path,
                                  clip_data_dir_name, escaped_name);
      g_array_append_vals (dst_data, file_uri, strlen (file_uri));

      g_free (file_uri);
      g_free (escaped_name);
      g_free (filename);
    }

  *dst_size = dst_data->len;

  return (uint8_t *) g_array_free (dst_data, FALSE);
}

static uint8_t *
convert_client_content_for_server (GrdClipboardRdp *clipboard_rdp,
                                   uint8_t         *src_data,
                                   uint32_t         src_size,
                                   GrdMimeType      mime_type,
                                   uint32_t         src_format_id,
                                   uint32_t        *dst_size,
                                   gboolean         has_clip_data_id,
                                   uint32_t         clip_data_id)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  uint32_t dst_format_id;
  uint8_t *dst_data;
  gboolean is_null_terminated = FALSE;
  BOOL success;

  *dst_size = 0;

  switch (mime_type)
    {
    case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
    case GRD_MIME_TYPE_TEXT_UTF8_STRING:
      is_null_terminated = TRUE;
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "UTF8_STRING");
      break;
    case GRD_MIME_TYPE_TEXT_HTML:
      is_null_terminated = TRUE;
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "HTML Format");
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "text/html");
      break;
    case GRD_MIME_TYPE_IMAGE_BMP:
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "image/bmp");
      break;
    case GRD_MIME_TYPE_TEXT_URILIST:
    case GRD_MIME_TYPE_XS_GNOME_COPIED_FILES:
      src_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "FileGroupDescriptorW");
      dst_format_id = ClipboardGetFormatId (clipboard_rdp->system,
                                            "text/uri-list");
      break;
    default:
      g_assert_not_reached ();
    }

  success = ClipboardSetData (clipboard_rdp->system,
                              src_format_id, src_data, src_size);
  if (!success)
    {
      g_warning ("[RDP.CLIPRDR] Converting clipboard content failed");
      return NULL;
    }

  if (mime_type == GRD_MIME_TYPE_TEXT_URILIST ||
      mime_type == GRD_MIME_TYPE_XS_GNOME_COPIED_FILES)
    {
      dst_data = get_uri_list_from_packet_file_list (clipboard_rdp,
                                                     src_data, src_size,
                                                     dst_size, has_clip_data_id,
                                                     clip_data_id);
    }
  else
    {
      dst_data = ClipboardGetData (clipboard_rdp->system, dst_format_id,
                                   dst_size);
    }
  if (!dst_data)
    {
      g_warning ("[RDP.CLIPRDR] Converting clipboard content failed");
      return NULL;
    }

  if (is_null_terminated)
    {
      uint8_t *null_terminator_pos = memchr (dst_data, '\0', *dst_size);
      if (null_terminator_pos)
        *dst_size = null_terminator_pos - dst_data;
    }

  if (mime_type == GRD_MIME_TYPE_TEXT_URILIST ||
      mime_type == GRD_MIME_TYPE_XS_GNOME_COPIED_FILES)
    {
      FILEDESCRIPTORW *files = NULL;
      FILEDESCRIPTORW *file;
      uint32_t n_files = 0;
      gboolean result;
      g_autofree char *clip_data_dir_name = NULL;
      char *filename = NULL;
      char *escaped_name;
      char *full_filepath;
      GrdMimeType second_mime_type;
      FormatData *format_data;
      GArray *data_nautilus;
      const char *nautilus_header;
      uint8_t *dst_data_nautilus;
      uint32_t dst_size_nautilus;
      uint32_t i;

      cliprdr_parse_file_list (src_data, src_size, &files, &n_files);
      if (has_clip_data_id)
        {
          result = grd_rdp_fuse_clipboard_set_cdi_selection (rdp_fuse_clipboard,
                                                             files, n_files,
                                                             clip_data_id);
        }
      else
        {
          result = grd_rdp_fuse_clipboard_set_no_cdi_selection (rdp_fuse_clipboard,
                                                                files, n_files);
        }
      if (!result)
        {
          g_free (files);
          g_free (dst_data);
          return NULL;
        }

      clip_data_dir_name = has_clip_data_id ?
        g_strdup_printf ("%u", clip_data_id) :
        g_strdup_printf ("%lu", GRD_RDP_FUSE_CLIPBOARD_NO_CLIP_DATA_ID);

      data_nautilus = g_array_new (TRUE, TRUE, sizeof (char));
      nautilus_header = "copy";
      g_array_append_vals (data_nautilus, nautilus_header,
                           strlen (nautilus_header));
      for (i = 0; i < n_files; ++i)
        {
          file = &files[i];

          if (ConvertFromUnicode (CP_UTF8, 0, file->cFileName, -1, &filename,
                                  0, NULL, NULL) <= 0)
            {
              g_array_free (data_nautilus, TRUE);
              grd_rdp_fuse_clipboard_clear_no_cdi_selection (rdp_fuse_clipboard);
              g_free (files);
              g_free (dst_data);
              return NULL;
            }
          if (strchr (filename, '\\') || strchr (filename, '/'))
            {
              g_free (filename);
              continue;
            }

          escaped_name = g_uri_escape_string (filename,
                           G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
          full_filepath = g_strdup_printf ("\nfile://%s/%s/%s",
                                           clipboard_rdp->fuse_mount_path,
                                           clip_data_dir_name, escaped_name);
          g_array_append_vals (data_nautilus, full_filepath,
                               strlen (full_filepath));

          g_free (full_filepath);
          g_free (escaped_name);
          g_free (filename);
        }

      dst_size_nautilus = data_nautilus->len;
      dst_data_nautilus = (uint8_t *) g_array_free (data_nautilus, FALSE);

      format_data = g_malloc0 (sizeof (FormatData));
      if (mime_type == GRD_MIME_TYPE_TEXT_URILIST)
        {
          /**
           * Also create the format data for the
           * "x-special/gnome-copied-files" mimetype
           */
          second_mime_type = GRD_MIME_TYPE_XS_GNOME_COPIED_FILES;
          format_data->size = dst_size_nautilus;
          format_data->data = dst_data_nautilus;
        }
      else if (mime_type == GRD_MIME_TYPE_XS_GNOME_COPIED_FILES)
        {
          /**
           * Also create the format data for the "text/uri-list" mimetype
           */
          second_mime_type = GRD_MIME_TYPE_TEXT_URILIST;
          format_data->size = *dst_size;
          format_data->data = dst_data;

          *dst_size = dst_size_nautilus;
          dst_data = dst_data_nautilus;
        }
      else
        {
          g_assert_not_reached ();
        }

      g_hash_table_insert (clipboard_rdp->format_data_cache,
                           GUINT_TO_POINTER (second_mime_type),
                           format_data);

      g_free (files);
    }

  if (!dst_data)
    g_warning ("[RDP.CLIPRDR] Converting clipboard content failed");

  return dst_data;
}

static gboolean
prepare_client_content_for_server (GrdClipboardRdp   *clipboard_rdp,
                                   uint8_t          **src_data,
                                   uint32_t           src_size,
                                   GrdMimeTypeTable  *mime_type_table,
                                   gboolean           has_clip_data_id,
                                   uint32_t           clip_data_id)
{
  GrdMimeType mime_type = mime_type_table->mime_type;
  uint32_t src_format_id = mime_type_table->rdp.format_id;
  FormatData *format_data;
  uint8_t *dst_data = NULL;
  uint32_t dst_size = 0;

  switch (mime_type)
    {
    case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
    case GRD_MIME_TYPE_TEXT_UTF8_STRING:
    case GRD_MIME_TYPE_TEXT_HTML:
    case GRD_MIME_TYPE_IMAGE_BMP:
    case GRD_MIME_TYPE_TEXT_URILIST:
    case GRD_MIME_TYPE_XS_GNOME_COPIED_FILES:
      dst_data = convert_client_content_for_server (clipboard_rdp,
                                                    *src_data, src_size,
                                                    mime_type, src_format_id,
                                                    &dst_size, has_clip_data_id,
                                                    clip_data_id);
      break;
    default:
      dst_data = g_steal_pointer (src_data);
      dst_size = src_size;
    }

  if (!dst_data)
    return FALSE;

  format_data = g_malloc0 (sizeof (FormatData));
  format_data->size = dst_size;
  format_data->data = dst_data;

  g_hash_table_insert (clipboard_rdp->format_data_cache,
                       GUINT_TO_POINTER (mime_type),
                       format_data);

  return TRUE;
}

static void
serve_mime_type_content (GrdClipboardRdp *clipboard_rdp,
                         GrdMimeType      mime_type)
{
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);
  FormatData *format_data;
  GList *serials;
  unsigned int serial;
  GList *l;

  if (!g_hash_table_steal_extended (clipboard_rdp->pending_client_requests,
                                    GUINT_TO_POINTER (mime_type),
                                    NULL, (gpointer *) &serials))
    return;

  if (!g_hash_table_lookup_extended (clipboard_rdp->format_data_cache,
                                     GUINT_TO_POINTER (mime_type),
                                     NULL, (gpointer *) &format_data))
    g_assert_not_reached ();

  for (l = serials; l; l = l->next)
    {
      serial = GPOINTER_TO_UINT (l->data);
      grd_clipboard_submit_client_content_for_mime_type (clipboard, serial,
                                                         format_data->data,
                                                         format_data->size);
    }
  g_list_free (serials);
}

static gboolean
handle_format_data_response (gpointer user_data)
{
  GrdClipboardRdp *clipboard_rdp = user_data;
  ClientFormatDataRequestContext *request_context;
  CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response;
  GrdMimeTypeTable *mime_type_table;
  GrdMimeType mime_type;
  uint8_t *src_data;
  uint32_t src_size;

  g_mutex_lock (&clipboard_rdp->client_request_mutex);
  request_context = g_steal_pointer (&clipboard_rdp->current_client_request);
  format_data_response = g_steal_pointer (&clipboard_rdp->format_data_response);

  clipboard_rdp->client_format_data_response_id = 0;
  g_mutex_unlock (&clipboard_rdp->client_request_mutex);

  mime_type_table = &request_context->mime_type_table;
  mime_type = mime_type_table->mime_type;

  if (extract_format_data_response (clipboard_rdp, format_data_response,
                                    &src_data, &src_size) &&
      prepare_client_content_for_server (clipboard_rdp, &src_data, src_size,
                                         mime_type_table,
                                         request_context->has_clip_data_id,
                                         request_context->clip_data_id))
    {
      serve_mime_type_content (clipboard_rdp, mime_type);

      if (mime_type == GRD_MIME_TYPE_TEXT_URILIST)
        serve_mime_type_content (clipboard_rdp, GRD_MIME_TYPE_XS_GNOME_COPIED_FILES);
      else if (mime_type == GRD_MIME_TYPE_XS_GNOME_COPIED_FILES)
        serve_mime_type_content (clipboard_rdp, GRD_MIME_TYPE_TEXT_URILIST);
    }
  else
    {
      abort_client_requests_for_context (clipboard_rdp, request_context);
    }
  g_free (src_data);
  g_free (format_data_response);
  g_free (request_context);

  g_clear_handle_id (&clipboard_rdp->client_request_abort_id, g_source_remove);

  maybe_send_next_mime_type_content_request (clipboard_rdp);

  return G_SOURCE_REMOVE;
}

/**
 * Clients response to our data request that we sent
 */
static uint32_t
cliprdr_client_format_data_response (CliprdrServerContext               *cliprdr_context,
                                     const CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;

  g_mutex_lock (&clipboard_rdp->client_request_mutex);
  if (!clipboard_rdp->current_client_request)
    {
      g_mutex_unlock (&clipboard_rdp->client_request_mutex);
      return CHANNEL_RC_OK;
    }

  if (clipboard_rdp->format_data_response)
    {
      g_free ((uint8_t *) clipboard_rdp->format_data_response->requestedFormatData);
      g_clear_pointer (&clipboard_rdp->format_data_response, g_free);
    }

  clipboard_rdp->format_data_response =
    g_memdup2 (format_data_response,
               sizeof (CLIPRDR_FORMAT_DATA_RESPONSE));
  clipboard_rdp->format_data_response->requestedFormatData =
    g_memdup2 (format_data_response->requestedFormatData,
               format_data_response->dataLen);

  if (!clipboard_rdp->client_format_data_response_id)
    {
      clipboard_rdp->client_format_data_response_id =
        g_idle_add (handle_format_data_response, clipboard_rdp);
    }
  g_mutex_unlock (&clipboard_rdp->client_request_mutex);

  return CHANNEL_RC_OK;
}

static uint32_t
delegate_request_file_contents_size (wClipboardDelegate                  *delegate,
                                     const CLIPRDR_FILE_CONTENTS_REQUEST *file_contents_request)
{
  wClipboardFileSizeRequest file_size_request = {0};

  file_size_request.streamId = file_contents_request->streamId;
  file_size_request.listIndex = file_contents_request->listIndex;

  return delegate->ClientRequestFileSize (delegate, &file_size_request);
}

static uint32_t
delegate_request_file_contents_range (wClipboardDelegate                  *delegate,
                                      const CLIPRDR_FILE_CONTENTS_REQUEST *file_contents_request)
{
  wClipboardFileRangeRequest file_range_request = {0};

  file_range_request.streamId = file_contents_request->streamId;
  file_range_request.listIndex = file_contents_request->listIndex;
  file_range_request.nPositionLow = file_contents_request->nPositionLow;
  file_range_request.nPositionHigh = file_contents_request->nPositionHigh;
  file_range_request.cbRequested = file_contents_request->cbRequested;

  return delegate->ClientRequestFileRange (delegate, &file_range_request);
}

static uint32_t
send_file_contents_response_failure (CliprdrServerContext *cliprdr_context,
                                     uint32_t              stream_id)
{
  CLIPRDR_FILE_CONTENTS_RESPONSE file_contents_response = {0};

  file_contents_response.msgType = CB_FILECONTENTS_RESPONSE;
  file_contents_response.msgFlags = CB_RESPONSE_FAIL;
  file_contents_response.streamId = stream_id;

  return cliprdr_context->ServerFileContentsResponse (cliprdr_context,
                                                      &file_contents_response);
}

/**
 * Client requests us to send either the size of the remote file or a portion
 * of the data in the file
 */
static uint32_t
cliprdr_client_file_contents_request (CliprdrServerContext                *cliprdr_context,
                                      const CLIPRDR_FILE_CONTENTS_REQUEST *file_contents_request)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  wClipboardDelegate *delegate = clipboard_rdp->delegate;
  gboolean has_file_list = clipboard_rdp->has_file_list;
  gboolean requests_allowed = clipboard_rdp->server_file_contents_requests_allowed;
  uint32_t clip_data_id = file_contents_request->clipDataId;
  uint32_t stream_id = file_contents_request->streamId;
  ClipDataEntry *entry = NULL;
  uint32_t error = NO_ERROR;

  if (file_contents_request->haveClipDataId)
    g_debug ("[RDP.CLIPRDR] FileContentsRequest has clipDataId %u", clip_data_id);
  else
    g_debug ("[RDP.CLIPRDR] FileContentsRequest does not have a clipDataId");

  if (file_contents_request->haveClipDataId)
    {
      if (!g_hash_table_lookup_extended (clipboard_rdp->clip_data_table,
                                         GUINT_TO_POINTER (clip_data_id),
                                         NULL, (gpointer *) &entry))
        return send_file_contents_response_failure (cliprdr_context, stream_id);

      delegate = entry->delegate;
      has_file_list = entry->has_file_list;
      requests_allowed = entry->requests_allowed;

      if (!requests_allowed)
        {
          g_debug ("[RDP.CLIPRDR] ClipDataEntry with id %u is not eligible of "
                   "requesting file contents.", clip_data_id);
        }
    }

  if (!requests_allowed || !has_file_list)
    return send_file_contents_response_failure (cliprdr_context, stream_id);

  if (file_contents_request->dwFlags & FILECONTENTS_SIZE &&
      file_contents_request->dwFlags & FILECONTENTS_RANGE)
    {
      /**
       * FILECONTENTS_SIZE and FILECONTENTS_RANGE are not allowed
       * to be set at the same time
       */
      return send_file_contents_response_failure (cliprdr_context, stream_id);
    }

  if (file_contents_request->dwFlags & FILECONTENTS_SIZE)
    {
      error = delegate_request_file_contents_size (delegate,
                                                   file_contents_request);
    }
  else if (file_contents_request->dwFlags & FILECONTENTS_RANGE)
    {
      error = delegate_request_file_contents_range (delegate,
                                                    file_contents_request);
    }
  else
    {
      error = ERROR_INVALID_DATA;
    }

  if (error)
    return send_file_contents_response_failure (cliprdr_context, stream_id);

  return CHANNEL_RC_OK;
}

/**
 * Clients response to our file contents request that we sent
 */
static uint32_t
cliprdr_client_file_contents_response (CliprdrServerContext                 *cliprdr_context,
                                       const CLIPRDR_FILE_CONTENTS_RESPONSE *file_contents_response)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;

  grd_rdp_fuse_clipboard_submit_file_contents_response (
    rdp_fuse_clipboard, file_contents_response->streamId,
    file_contents_response->msgFlags & CB_RESPONSE_OK,
    file_contents_response->requestedData,
    file_contents_response->cbRequested);

  return CHANNEL_RC_OK;
}

static uint32_t
cliprdr_file_size_success (wClipboardDelegate              *delegate,
                           const wClipboardFileSizeRequest *file_size_request,
                           uint64_t                         file_size)
{
  GrdClipboardRdp *clipboard_rdp = delegate->custom;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FILE_CONTENTS_RESPONSE file_contents_response = {0};

  file_contents_response.msgType = CB_FILECONTENTS_RESPONSE;
  file_contents_response.msgFlags = CB_RESPONSE_OK;
  file_contents_response.streamId = file_size_request->streamId;
  file_contents_response.cbRequested = sizeof (uint64_t);
  file_contents_response.requestedData = (uint8_t *) &file_size;

  return cliprdr_context->ServerFileContentsResponse (cliprdr_context,
                                                      &file_contents_response);
}

static uint32_t
cliprdr_file_size_failure (wClipboardDelegate              *delegate,
                           const wClipboardFileSizeRequest *file_size_request,
                           uint32_t                         error)
{
  GrdClipboardRdp *clipboard_rdp = delegate->custom;

  return send_file_contents_response_failure (clipboard_rdp->cliprdr_context,
                                              file_size_request->streamId);
}

static uint32_t
cliprdr_file_range_success (wClipboardDelegate               *delegate,
                            const wClipboardFileRangeRequest *file_range_request,
                            const uint8_t                    *data,
                            uint32_t                          size)
{
  GrdClipboardRdp *clipboard_rdp = delegate->custom;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FILE_CONTENTS_RESPONSE file_contents_response = {0};

  file_contents_response.msgType = CB_FILECONTENTS_RESPONSE;
  file_contents_response.msgFlags = CB_RESPONSE_OK;
  file_contents_response.streamId = file_range_request->streamId;
  file_contents_response.cbRequested = size;
  file_contents_response.requestedData = data;

  return cliprdr_context->ServerFileContentsResponse (cliprdr_context,
                                                      &file_contents_response);
}

static uint32_t
cliprdr_file_range_failure (wClipboardDelegate               *delegate,
                            const wClipboardFileRangeRequest *file_range_request,
                            uint32_t                          error)
{
  GrdClipboardRdp *clipboard_rdp = delegate->custom;

  return send_file_contents_response_failure (clipboard_rdp->cliprdr_context,
                                              file_range_request->streamId);
}

static BOOL
is_valid_unix_filename (LPCWSTR filename)
{
  LPCWSTR c;

  if (!filename)
    return FALSE;

  if (filename[0] == L'\0')
    return FALSE;

  /* Reserved characters */
  for (c = filename; *c; ++c)
    {
      if (*c == L'/')
        return FALSE;
    }

  return TRUE;
}

static void
create_new_winpr_clipboard (GrdClipboardRdp *clipboard_rdp)
{
  g_debug ("[RDP.CLIPRDR] Creating new WinPR clipboard");

  clipboard_rdp->system = ClipboardCreate ();
  clipboard_rdp->delegate = ClipboardGetDelegate (clipboard_rdp->system);
  clipboard_rdp->delegate->ClipboardFileSizeSuccess = cliprdr_file_size_success;
  clipboard_rdp->delegate->ClipboardFileSizeFailure = cliprdr_file_size_failure;
  clipboard_rdp->delegate->ClipboardFileRangeSuccess = cliprdr_file_range_success;
  clipboard_rdp->delegate->ClipboardFileRangeFailure = cliprdr_file_range_failure;
  clipboard_rdp->delegate->basePath = NULL;
  clipboard_rdp->delegate->custom = clipboard_rdp;

  if (clipboard_rdp->relieve_filename_restriction)
    clipboard_rdp->delegate->IsFileNameComponentValid = is_valid_unix_filename;

  clipboard_rdp->has_file_list = FALSE;
}

GrdClipboardRdp *
grd_clipboard_rdp_new (GrdSessionRdp *session_rdp,
                       HANDLE         vcm,
                       gboolean       relieve_filename_restriction)
{
  g_autoptr (GrdClipboardRdp) clipboard_rdp = NULL;
  GrdClipboard *clipboard;
  CliprdrServerContext *cliprdr_context;

  clipboard_rdp = g_object_new (GRD_TYPE_CLIPBOARD_RDP, NULL);
  cliprdr_context = cliprdr_server_context_new (vcm);
  if (!cliprdr_context)
    {
      g_warning ("[RDP.CLIPRDR] Failed to create server context");
      return NULL;
    }

  clipboard_rdp->cliprdr_context = cliprdr_context;
  clipboard_rdp->relieve_filename_restriction = relieve_filename_restriction;

  clipboard = GRD_CLIPBOARD (clipboard_rdp);
  grd_clipboard_initialize (clipboard, GRD_SESSION (session_rdp));

  cliprdr_context->useLongFormatNames = TRUE;
  cliprdr_context->streamFileClipEnabled = TRUE;
  cliprdr_context->fileClipNoFilePaths = TRUE;
  cliprdr_context->canLockClipData = TRUE;
  cliprdr_context->hasHugeFileSupport = TRUE;

  cliprdr_context->ClientCapabilities = cliprdr_client_capabilities;
  cliprdr_context->TempDirectory = cliprdr_temp_directory;
  cliprdr_context->ClientFormatList = cliprdr_client_format_list;
  cliprdr_context->ClientFormatListResponse = cliprdr_client_format_list_response;
  cliprdr_context->ClientLockClipboardData = cliprdr_client_lock_clipboard_data;
  cliprdr_context->ClientUnlockClipboardData = cliprdr_client_unlock_clipboard_data;
  cliprdr_context->ClientFormatDataRequest = cliprdr_client_format_data_request;
  cliprdr_context->ClientFormatDataResponse = cliprdr_client_format_data_response;
  cliprdr_context->ClientFileContentsRequest = cliprdr_client_file_contents_request;
  cliprdr_context->ClientFileContentsResponse = cliprdr_client_file_contents_response;
  cliprdr_context->custom = clipboard_rdp;

  if (relieve_filename_restriction)
    {
      g_message ("[RDP.CLIPRDR] Relieving CLIPRDR filename restriction");
      clipboard_rdp->delegate->IsFileNameComponentValid = is_valid_unix_filename;
    }

  if (cliprdr_context->Start (cliprdr_context))
    {
      g_warning ("[RDP.CLIPRDR] Failed to open CLIPRDR channel");
      g_clear_pointer (&clipboard_rdp->cliprdr_context,
                       cliprdr_server_context_free);
      return NULL;
    }

  return g_steal_pointer (&clipboard_rdp);
}

static gboolean
clear_format_data (gpointer key,
                   gpointer value,
                   gpointer user_data)
{
  FormatData *format_data = value;

  g_free (format_data->data);
  g_free (format_data);

  return TRUE;
}

static gboolean
clear_client_requests (gpointer key,
                       gpointer value,
                       gpointer user_data)
{
  GList *serials = value;

  g_list_free (serials);

  return TRUE;
}

static void
grd_clipboard_rdp_dispose (GObject *object)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (object);
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);

  g_mutex_lock (&clipboard_rdp->completion_mutex);
  clipboard_rdp->protocol_stopped = TRUE;
  g_cond_signal (&clipboard_rdp->completion_cond);
  g_mutex_unlock (&clipboard_rdp->completion_mutex);

  if (clipboard_rdp->cliprdr_context)
    {
      clipboard_rdp->cliprdr_context->Stop (clipboard_rdp->cliprdr_context);
      grd_clipboard_disable_clipboard (clipboard);
    }

  g_clear_pointer (&clipboard_rdp->current_client_request, g_free);
  if (clipboard_rdp->format_data_response)
    {
      g_free ((uint8_t *) clipboard_rdp->format_data_response->requestedFormatData);
      g_clear_pointer (&clipboard_rdp->format_data_response, g_free);
    }

  if (clipboard_rdp->clipboard_retrieval_id)
    g_clear_pointer (&clipboard_rdp->clipboard_retrieval_context.entry, g_free);

  g_clear_pointer (&clipboard_rdp->format_data_request_context, g_free);
  g_clear_pointer (&clipboard_rdp->queued_server_formats, g_list_free);
  g_clear_pointer (&clipboard_rdp->pending_server_formats, g_list_free);

  if (clipboard_rdp->ordered_client_requests)
    {
      g_queue_free_full (clipboard_rdp->ordered_client_requests, g_free);
      clipboard_rdp->ordered_client_requests = NULL;
    }
  g_hash_table_foreach_remove (clipboard_rdp->pending_client_requests,
                               clear_client_requests,
                               NULL);
  g_hash_table_foreach_remove (clipboard_rdp->format_data_cache,
                               clear_format_data,
                               NULL);

  g_clear_object (&clipboard_rdp->rdp_fuse_clipboard);
  rmdir (clipboard_rdp->fuse_mount_path);
  g_clear_pointer (&clipboard_rdp->fuse_mount_path, g_free);

  g_assert (g_hash_table_size (clipboard_rdp->pending_client_requests) == 0);
  g_assert (g_hash_table_size (clipboard_rdp->format_data_cache) == 0);
  g_clear_pointer (&clipboard_rdp->pending_client_requests, g_hash_table_unref);
  g_clear_pointer (&clipboard_rdp->format_data_cache, g_hash_table_unref);
  g_clear_pointer (&clipboard_rdp->clip_data_table, g_hash_table_destroy);
  g_clear_pointer (&clipboard_rdp->serial_entry_table, g_hash_table_destroy);
  g_clear_pointer (&clipboard_rdp->allowed_server_formats, g_hash_table_destroy);
  g_clear_pointer (&clipboard_rdp->system, ClipboardDestroy);
  g_clear_pointer (&clipboard_rdp->cliprdr_context, cliprdr_server_context_free);

  g_clear_handle_id (&clipboard_rdp->pending_server_formats_drop_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->client_request_abort_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->clipboard_retrieval_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->clipboard_destruction_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->server_format_list_update_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->server_format_data_request_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->client_format_list_response_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->client_format_data_response_id, g_source_remove);

  G_OBJECT_CLASS (grd_clipboard_rdp_parent_class)->dispose (object);
}

static void
grd_clipboard_rdp_finalize (GObject *object)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (object);

  g_cond_clear (&clipboard_rdp->completion_cond);
  g_mutex_clear (&clipboard_rdp->completion_mutex);
  g_mutex_clear (&clipboard_rdp->server_format_data_request_mutex);
  g_mutex_clear (&clipboard_rdp->client_format_list_response_mutex);
  g_mutex_clear (&clipboard_rdp->server_format_list_update_mutex);
  g_mutex_clear (&clipboard_rdp->clip_data_entry_mutex);
  g_mutex_clear (&clipboard_rdp->client_request_mutex);

  G_OBJECT_CLASS (grd_clipboard_rdp_parent_class)->finalize (object);
}

static void
clip_data_entry_free (gpointer data)
{
  ClipDataEntry *entry = data;

  g_debug ("[RDP.CLIPRDR] Freeing ClipDataEntry with id %u and serial %lu. "
           "ClipDataEntry is independent: %s", entry->id, entry->serial,
           entry->is_independent ? "true" : "false");

  if (entry->is_independent)
    g_clear_pointer (&entry->system, ClipboardDestroy);

  g_free (entry);
}

static void
grd_clipboard_rdp_init (GrdClipboardRdp *clipboard_rdp)
{
  const char *grd_path = "/gnome-remote-desktop";
  const char *cliprdr_template = "/cliprdr-XXXXXX";
  g_autofree char *base_path = NULL;
  g_autofree char *template_path = NULL;

  base_path = g_strdup_printf ("%s%s", g_get_user_runtime_dir (), grd_path);
  template_path = g_strdup_printf ("%s%s", base_path, cliprdr_template);

  if (g_access (base_path, F_OK))
    {
      if (mkdir (base_path, 0700))
        {
          g_error ("Failed to create base runtime directory for "
                   "gnome-remote-desktop: %s", g_strerror (errno));
        }
    }
  if (!mkdtemp (template_path))
    {
      g_error ("Failed to create clipboard file directory %s: %s",
               template_path, g_strerror (errno));
    }

  clipboard_rdp->system = ClipboardCreate ();
  clipboard_rdp->delegate = ClipboardGetDelegate (clipboard_rdp->system);
  clipboard_rdp->delegate->ClipboardFileSizeSuccess = cliprdr_file_size_success;
  clipboard_rdp->delegate->ClipboardFileSizeFailure = cliprdr_file_size_failure;
  clipboard_rdp->delegate->ClipboardFileRangeSuccess = cliprdr_file_range_success;
  clipboard_rdp->delegate->ClipboardFileRangeFailure = cliprdr_file_range_failure;
  clipboard_rdp->delegate->basePath = NULL;
  clipboard_rdp->delegate->custom = clipboard_rdp;

  clipboard_rdp->completed_clip_data_entry = TRUE;
  clipboard_rdp->completed_format_list = TRUE;
  clipboard_rdp->completed_format_data_request = TRUE;

  clipboard_rdp->allowed_server_formats = g_hash_table_new (NULL, NULL);
  clipboard_rdp->serial_entry_table = g_hash_table_new_full (NULL, NULL, NULL,
                                                             clip_data_entry_free);
  clipboard_rdp->clip_data_table = g_hash_table_new (NULL, NULL);
  clipboard_rdp->format_data_cache = g_hash_table_new (NULL, NULL);
  clipboard_rdp->pending_client_requests = g_hash_table_new (NULL, NULL);
  clipboard_rdp->ordered_client_requests = g_queue_new ();

  g_mutex_init (&clipboard_rdp->client_request_mutex);
  g_mutex_init (&clipboard_rdp->clip_data_entry_mutex);
  g_mutex_init (&clipboard_rdp->server_format_list_update_mutex);
  g_mutex_init (&clipboard_rdp->client_format_list_response_mutex);
  g_mutex_init (&clipboard_rdp->server_format_data_request_mutex);
  g_mutex_init (&clipboard_rdp->completion_mutex);
  g_cond_init (&clipboard_rdp->completion_cond);

  clipboard_rdp->fuse_mount_path = g_steal_pointer (&template_path);
  clipboard_rdp->rdp_fuse_clipboard =
    grd_rdp_fuse_clipboard_new (clipboard_rdp, clipboard_rdp->fuse_mount_path);
}

static void
grd_clipboard_rdp_class_init (GrdClipboardRdpClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdClipboardClass *clipboard_class = GRD_CLIPBOARD_CLASS (klass);

  object_class->dispose = grd_clipboard_rdp_dispose;
  object_class->finalize = grd_clipboard_rdp_finalize;

  clipboard_class->update_client_mime_type_list =
    grd_clipboard_rdp_update_client_mime_type_list;
  clipboard_class->request_client_content_for_mime_type =
    grd_clipboard_rdp_request_client_content_for_mime_type;
  clipboard_class->submit_requested_server_content =
    grd_clipboard_rdp_submit_requested_server_content;
}
