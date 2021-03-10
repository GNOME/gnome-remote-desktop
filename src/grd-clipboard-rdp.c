/*
 * Copyright (C) 2020 Pascal Nowack
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

typedef struct _FormatData
{
  uint8_t *data;
  uint32_t size;
} FormatData;

struct _GrdClipboardRdp
{
  GrdClipboard parent;

  CliprdrServerContext *cliprdr_context;
  HANDLE stop_event;

  wClipboard *system;
  wClipboardDelegate *delegate;

  GHashTable *allowed_server_formats;
  GList *pending_server_formats;
  GList *queued_server_formats;
  gboolean server_file_contents_requests_allowed;
  uint16_t format_list_response_msg_flags;

  char *fuse_mount_path;
  GrdRdpFuseClipboard *rdp_fuse_clipboard;
  GHashTable *format_data_cache;
  GrdMimeType which_unicode_format;

  HANDLE format_data_received_event;
  CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response;

  HANDLE completed_format_list_event;
  HANDLE completed_format_data_request_event;
  HANDLE format_list_received_event;
  HANDLE format_list_response_received_event;
  HANDLE format_data_request_received_event;
  unsigned int pending_server_formats_drop_id;
  unsigned int server_format_list_update_id;
  unsigned int server_format_data_request_id;
  unsigned int client_format_list_response_id;
};

G_DEFINE_TYPE (GrdClipboardRdp, grd_clipboard_rdp, GRD_TYPE_CLIPBOARD);

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
            clipboard_rdp->server_file_contents_requests_allowed = TRUE;

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
send_mime_type_list (GrdClipboardRdp *clipboard_rdp,
                     GList           *mime_type_list)
{
  GrdRdpFuseClipboard *rdp_fuse_clipboard = clipboard_rdp->rdp_fuse_clipboard;
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_LIST format_list = {0};
  CLIPRDR_FORMAT *cliprdr_formats;
  GrdMimeType mime_type;
  uint32_t n_formats;
  uint32_t i;
  GList *l;

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
          grd_rdp_fuse_clipboard_clear_selection (rdp_fuse_clipboard);
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

static CLIPRDR_FORMAT_DATA_RESPONSE *
get_format_data_response (GrdClipboardRdp             *clipboard_rdp,
                          CLIPRDR_FORMAT_DATA_REQUEST *format_data_request)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response = NULL;
  HANDLE format_data_received_event = clipboard_rdp->format_data_received_event;
  HANDLE events[3];
  uint32_t error;

  ResetEvent (format_data_received_event);
  error = cliprdr_context->ServerFormatDataRequest (cliprdr_context,
                                                    format_data_request);
  if (error)
    return NULL;

  events[0] = clipboard_rdp->stop_event;
  events[1] = format_data_received_event;
  events[2] = clipboard_rdp->format_list_received_event;
  if (WaitForMultipleObjects (3, events, FALSE, MAX_WAIT_TIME) == WAIT_TIMEOUT)
    {
      g_warning ("[RDP.CLIPRDR] Possible protocol violation: Client did not "
                 "send format data response (Timeout reached)");
    }

  if (WaitForSingleObject (format_data_received_event, 0) == WAIT_OBJECT_0)
    {
      format_data_response =
        g_steal_pointer (&clipboard_rdp->format_data_response);
    }

  return format_data_response;
}

static uint8_t *
get_remote_format_data (GrdClipboardRdp *clipboard_rdp,
                        uint32_t         format_id,
                        uint32_t        *size)
{
  CLIPRDR_FORMAT_DATA_REQUEST format_data_request = {0};
  CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response;
  uint8_t *data;
  gboolean response_ok;

  *size = 0;

  format_data_request.msgType = CB_FORMAT_DATA_REQUEST;
  format_data_request.dataLen = 4;
  format_data_request.requestedFormatId = format_id;
  format_data_response = get_format_data_response (clipboard_rdp,
                                                   &format_data_request);
  if (!format_data_response)
    return NULL;

  data = (uint8_t *) format_data_response->requestedFormatData;
  *size = format_data_response->dataLen;

  response_ok = format_data_response->msgFlags & CB_RESPONSE_OK;
  *size = response_ok && data ? *size : 0;
  g_free (format_data_response);

  if (!response_ok)
    g_clear_pointer (&data, g_free);

  return data;
}

void
grd_clipboard_rdp_request_remote_file_size_async (GrdClipboardRdp *clipboard_rdp,
                                                  uint32_t         stream_id,
                                                  uint32_t         list_index)
{
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FILE_CONTENTS_REQUEST file_contents_request = {0};

  file_contents_request.msgType = CB_FILECONTENTS_REQUEST;
  file_contents_request.streamId = stream_id;
  file_contents_request.listIndex = list_index;
  file_contents_request.dwFlags = FILECONTENTS_SIZE;
  file_contents_request.cbRequested = 0x8;

  cliprdr_context->ServerFileContentsRequest (cliprdr_context,
                                              &file_contents_request);
}

void
grd_clipboard_rdp_request_remote_file_range_async (GrdClipboardRdp *clipboard_rdp,
                                                   uint32_t         stream_id,
                                                   uint32_t         list_index,
                                                   uint64_t         offset,
                                                   uint32_t         requested_size)
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
  file_contents_request.haveClipDataId = FALSE;

  cliprdr_context->ServerFileContentsRequest (cliprdr_context,
                                              &file_contents_request);
}

static uint8_t *
get_uri_list_from_packet_file_list (GrdClipboardRdp *clipboard_rdp,
                                    uint8_t         *src_data,
                                    uint32_t         src_size,
                                    uint32_t        *dst_size)
{
  FILEDESCRIPTOR *files = NULL;
  FILEDESCRIPTOR *file;
  uint32_t n_files = 0;
  char *filename = NULL;
  char *escaped_name;
  char *file_uri;
  GArray *dst_data;
  uint32_t i;

  dst_data = g_array_new (TRUE, TRUE, sizeof (char));

  cliprdr_parse_file_list (src_data, src_size, &files, &n_files);
  for (i = 0; i < n_files; ++i)
    {
      file = &files[i];

      ConvertFromUnicode (CP_UTF8, 0, file->cFileName, -1, &filename, 0, NULL,
                          NULL);
      if (strchr (filename, '\\'))
        {
          g_free (filename);
          continue;
        }

      escaped_name = g_uri_escape_string (filename,
                                          G_URI_RESERVED_CHARS_ALLOWED_IN_PATH,
                                          TRUE);
      file_uri = g_malloc0 (strlen ("file://") +
                            strlen (clipboard_rdp->fuse_mount_path) + 1 +
                            strlen (escaped_name) + 2 + 1);
      sprintf (file_uri, "file://%s/%s\r\n", clipboard_rdp->fuse_mount_path,
               escaped_name);
      g_array_append_vals (dst_data, file_uri, strlen (file_uri));

      g_free (file_uri);
      g_free (escaped_name);
      g_free (filename);
    }

  *dst_size = dst_data->len;

  g_free (files);

  return (uint8_t *) g_array_free (dst_data, FALSE);
}

static uint8_t *
convert_client_content_for_server (GrdClipboardRdp *clipboard_rdp,
                                   uint8_t         *src_data,
                                   uint32_t         src_size,
                                   GrdMimeType      mime_type,
                                   uint32_t         src_format_id,
                                   uint32_t        *dst_size)
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
                                                     dst_size);
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
      FILEDESCRIPTOR *files = NULL;
      FILEDESCRIPTOR *file;
      uint32_t n_files = 0;
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
      if (!grd_rdp_fuse_clipboard_set_selection (rdp_fuse_clipboard, files, n_files))
        {
          g_free (files);
          g_free (dst_data);
          return NULL;
        }

      data_nautilus = g_array_new (TRUE, TRUE, sizeof (char));
      nautilus_header = "copy";
      g_array_append_vals (data_nautilus, nautilus_header,
                           strlen (nautilus_header));
      for (i = 0; i < n_files; ++i)
        {
          file = &files[i];

          ConvertFromUnicode (CP_UTF8, 0, file->cFileName, -1, &filename,
                              0, NULL, NULL);
          if (strchr (filename, '\\'))
            {
              g_free (filename);
              continue;
            }

          escaped_name = g_uri_escape_string (filename,
                           G_URI_RESERVED_CHARS_ALLOWED_IN_PATH, TRUE);
          full_filepath = g_malloc0 (strlen ("\nfile://") +
                                     strlen (clipboard_rdp->fuse_mount_path) + 1 +
                                     strlen (escaped_name) + 1);
          sprintf (full_filepath, "\nfile://%s/%s",
                    clipboard_rdp->fuse_mount_path, escaped_name);
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

static uint8_t *
grd_clipboard_rdp_request_client_content_for_mime_type (GrdClipboard     *clipboard,
                                                        GrdMimeTypeTable *mime_type_table,
                                                        uint32_t         *size)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (clipboard);
  GrdMimeType mime_type = mime_type_table->mime_type;
  HANDLE completed_format_list_event = clipboard_rdp->completed_format_list_event;
  uint32_t src_format_id;
  uint8_t *src_data, *dst_data;
  uint32_t src_size, dst_size;

  *size = 0;
  if (mime_type == GRD_MIME_TYPE_NONE)
    return NULL;

  if (g_hash_table_contains (clipboard_rdp->format_data_cache,
                             GUINT_TO_POINTER (mime_type)))
    {
      FormatData *format_data;

      format_data = g_hash_table_lookup (clipboard_rdp->format_data_cache,
                                         GUINT_TO_POINTER (mime_type));
      *size = format_data->size;

      return g_memdup2 (format_data->data, format_data->size);
    }

  if (WaitForSingleObject (completed_format_list_event, 0) == WAIT_TIMEOUT)
    return NULL;

  src_format_id = mime_type_table->rdp.format_id;
  src_data = get_remote_format_data (clipboard_rdp, src_format_id, &src_size);
  if (!src_data)
    return NULL;

  dst_data = NULL;
  dst_size = 0;

  switch (mime_type)
    {
    case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
    case GRD_MIME_TYPE_TEXT_UTF8_STRING:
    case GRD_MIME_TYPE_TEXT_HTML:
    case GRD_MIME_TYPE_IMAGE_BMP:
    case GRD_MIME_TYPE_TEXT_URILIST:
    case GRD_MIME_TYPE_XS_GNOME_COPIED_FILES:
      dst_data = convert_client_content_for_server (clipboard_rdp,
                                                    src_data, src_size,
                                                    mime_type, src_format_id,
                                                    &dst_size);
      break;
    default:
      dst_data = g_steal_pointer (&src_data);
      dst_size = src_size;
    }

  if (dst_data)
    {
      FormatData *format_data = g_malloc0 (sizeof (FormatData));

      format_data->size = *size = dst_size;
      format_data->data = dst_data;

      g_hash_table_insert (clipboard_rdp->format_data_cache,
                           GUINT_TO_POINTER (mime_type),
                           format_data);
    }

  g_free (src_data);

  return g_memdup2 (dst_data, dst_size);
}

/**
 * FreeRDP already updated our capabilites after the client told us
 * about its capabilities, there is nothing to do here
 */
static uint32_t
cliprdr_client_capabilities (CliprdrServerContext       *cliprdr_context,
                             const CLIPRDR_CAPABILITIES *capabilities)
{
  return CHANNEL_RC_OK;
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
  GList *mime_type_tables = update_context->mime_type_tables;
  GrdMimeTypeTable *mime_type_table;
  GList *l;

  for (l = mime_type_tables; l; l = l->next)
    {
      mime_type_table = l->data;

      /* Also indirectly handles the GRD_MIME_TYPE_XS_GNOME_COPIED_FILES case */
      if (mime_type_table->mime_type == GRD_MIME_TYPE_TEXT_URILIST)
        {
          clipboard_rdp->server_file_contents_requests_allowed = FALSE;
          grd_rdp_fuse_clipboard_clear_selection (rdp_fuse_clipboard);
        }

      remove_clipboard_format_data_for_mime_type (clipboard_rdp,
                                                  mime_type_table->mime_type);
      g_hash_table_remove (clipboard_rdp->allowed_server_formats,
                           GUINT_TO_POINTER (mime_type_table->mime_type));
    }

  grd_clipboard_update_server_mime_type_list (clipboard, mime_type_tables);

  format_list_response.msgType = CB_FORMAT_LIST_RESPONSE;
  format_list_response.msgFlags = CB_RESPONSE_OK;

  cliprdr_context->ServerFormatListResponse (cliprdr_context,
                                             &format_list_response);

  /**
   * Any FileContentsRequest that is still waiting for a FileContentsResponse
   * won't get a response any more. So, drop all requests here.
   */
  grd_rdp_fuse_clipboard_dismiss_all_requests (rdp_fuse_clipboard);

  g_free (update_context);

  WaitForSingleObject (clipboard_rdp->format_list_received_event, INFINITE);
  clipboard_rdp->server_format_list_update_id = 0;
  ResetEvent (clipboard_rdp->format_list_received_event);
  SetEvent (clipboard_rdp->completed_format_list_event);

  return G_SOURCE_REMOVE;
}

/**
 * Client informs us that its clipboard is updated with new clipboard data
 */
static uint32_t
cliprdr_client_format_list (CliprdrServerContext      *cliprdr_context,
                            const CLIPRDR_FORMAT_LIST *format_list)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;
  ServerFormatListUpdateContext *update_context;
  GrdMimeTypeTable *mime_type_table = NULL;
  GList *mime_type_tables = NULL;
  GrdMimeType mime_type;
  gboolean already_has_text_format = FALSE;
  HANDLE events[2];
  uint32_t i;

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

          mime_type = GRD_MIME_TYPE_TEXT_UTF8_STRING;

          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

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
          /**
           * Advertise the "x-special/gnome-copied-files" format in addition to
           * the "text/uri-list" format
           */
          mime_type = GRD_MIME_TYPE_XS_GNOME_COPIED_FILES;

          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);

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

                  mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
                  mime_type_table->mime_type = mime_type;
                  mime_type_table->rdp.format_id = format_list->formats[i].formatId;
                  mime_type_tables = g_list_append (mime_type_tables,
                                                    mime_type_table);

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

      if (mime_type != GRD_MIME_TYPE_NONE)
        {
          mime_type_table = g_malloc0 (sizeof (GrdMimeTypeTable));
          mime_type_table->mime_type = mime_type;
          mime_type_table->rdp.format_id = format_list->formats[i].formatId;
          mime_type_tables = g_list_append (mime_type_tables, mime_type_table);
        }
    }

  if (clipboard_rdp->server_format_list_update_id)
    {
      g_debug ("[RDP.CLIPRDR] Wrong message sequence: Got new format list "
               "without being able to response to last update first");
    }

  events[0] = clipboard_rdp->stop_event;
  events[1] = clipboard_rdp->completed_format_list_event;
  WaitForMultipleObjects (2, events, FALSE, INFINITE);

  if (WaitForSingleObject (clipboard_rdp->stop_event, 0) == WAIT_OBJECT_0)
    {
      g_list_free_full (mime_type_tables, g_free);
      return CHANNEL_RC_OK;
    }

  ResetEvent (clipboard_rdp->completed_format_list_event);
  update_context = g_malloc0 (sizeof (ServerFormatListUpdateContext));
  update_context->clipboard_rdp = clipboard_rdp;
  update_context->mime_type_tables = mime_type_tables;

  clipboard_rdp->server_format_list_update_id =
    g_timeout_add (0, update_server_format_list, update_context);
  SetEvent (clipboard_rdp->format_list_received_event);

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

  WaitForSingleObject (clipboard_rdp->format_list_response_received_event,
                       INFINITE);
  clipboard_rdp->client_format_list_response_id = 0;
  ResetEvent (clipboard_rdp->format_list_response_received_event);

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
  HANDLE format_list_response_received_event;

  format_list_response_received_event =
    clipboard_rdp->format_list_response_received_event;

  if (WaitForSingleObject (format_list_response_received_event, 0) == WAIT_OBJECT_0)
    {
      g_warning ("[RDP.CLIPRDR] Wrong message sequence: Received an unexpected "
                 "format list response. Ignoring...");
      return CHANNEL_RC_OK;
    }

  clipboard_rdp->format_list_response_msg_flags = format_list_response->msgFlags;
  clipboard_rdp->client_format_list_response_id =
    g_timeout_add (0, handle_format_list_response, clipboard_rdp);
  SetEvent (format_list_response_received_event);

  return CHANNEL_RC_OK;
}

static void
serialize_file_list (FILEDESCRIPTOR  *files,
                     uint32_t         n_files,
                     uint8_t        **dst_data,
                     uint32_t        *dst_size)
{
  FILEDESCRIPTOR *file;
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

static gboolean
request_server_format_data (gpointer user_data)
{
  ServerFormatDataRequestContext *request_context = user_data;
  GrdClipboardRdp *clipboard_rdp = request_context->clipboard_rdp;
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);
  CliprdrServerContext *cliprdr_context = clipboard_rdp->cliprdr_context;
  CLIPRDR_FORMAT_DATA_RESPONSE format_data_response = {0};
  GrdMimeType mime_type = request_context->mime_type;
  uint32_t src_format_id = request_context->src_format_id;
  uint32_t dst_format_id = request_context->dst_format_id;
  uint8_t *src_data, *dst_data;
  uint32_t src_size, dst_size;
  BOOL success;

  dst_data = src_data = NULL;
  dst_size = src_size = 0;

  if (g_hash_table_contains (clipboard_rdp->allowed_server_formats,
                             GUINT_TO_POINTER (mime_type)))
    {
      src_data = grd_clipboard_request_server_content_for_mime_type (clipboard,
                                                                     mime_type,
                                                                     &src_size);
    }
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
                  FILEDESCRIPTOR *files;
                  uint32_t n_files;

                  files = (FILEDESCRIPTOR *) dst_data;
                  n_files = dst_size / sizeof (FILEDESCRIPTOR);

                  dst_data = NULL;
                  dst_size = 0;
                  serialize_file_list (files, n_files, &dst_data, &dst_size);

                  g_free (files);
                }
            }
          if (!success || !dst_data)
            g_warning ("[RDP.CLIPRDR] Converting clipboard content failed");
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

  WaitForSingleObject (clipboard_rdp->format_data_request_received_event,
                       INFINITE);
  clipboard_rdp->server_format_data_request_id = 0;
  ResetEvent (clipboard_rdp->format_data_request_received_event);
  SetEvent (clipboard_rdp->completed_format_data_request_event);

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
  HANDLE events[2];

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

  events[0] = clipboard_rdp->stop_event;
  events[1] = clipboard_rdp->completed_format_data_request_event;
  WaitForMultipleObjects (2, events, FALSE, INFINITE);

  if (WaitForSingleObject (clipboard_rdp->stop_event, 0) == WAIT_OBJECT_0)
    return CHANNEL_RC_OK;

  if (mime_type != GRD_MIME_TYPE_NONE)
    {
      ResetEvent (clipboard_rdp->completed_format_data_request_event);
      request_context = g_malloc0 (sizeof (ServerFormatDataRequestContext));
      request_context->clipboard_rdp = clipboard_rdp;
      request_context->mime_type = mime_type;
      request_context->src_format_id = src_format_id;
      request_context->dst_format_id = dst_format_id;
      request_context->needs_null_terminator = needs_null_terminator;
      request_context->needs_conversion = needs_conversion;

      clipboard_rdp->server_format_data_request_id =
        g_timeout_add (0, request_server_format_data, request_context);
      SetEvent (clipboard_rdp->format_data_request_received_event);

      return CHANNEL_RC_OK;
    }

  format_data_response.msgType = CB_FORMAT_DATA_RESPONSE;
  format_data_response.msgFlags = CB_RESPONSE_FAIL;
  format_data_response.dataLen = 0;
  format_data_response.requestedFormatData = NULL;

  return cliprdr_context->ServerFormatDataResponse (cliprdr_context,
                                                    &format_data_response);
}

/**
 * Clients response to our data request that we sent
 */
static uint32_t
cliprdr_client_format_data_response (CliprdrServerContext               *cliprdr_context,
                                     const CLIPRDR_FORMAT_DATA_RESPONSE *format_data_response)
{
  GrdClipboardRdp *clipboard_rdp = cliprdr_context->custom;

  if (clipboard_rdp->format_data_response)
    g_free ((uint8_t *) clipboard_rdp->format_data_response->requestedFormatData);
  g_clear_pointer (&clipboard_rdp->format_data_response, g_free);

  clipboard_rdp->format_data_response =
    g_memdup2 (format_data_response,
               sizeof (CLIPRDR_FORMAT_DATA_RESPONSE));
  clipboard_rdp->format_data_response->requestedFormatData =
    g_memdup2 (format_data_response->requestedFormatData,
               format_data_response->dataLen);
  SetEvent (clipboard_rdp->format_data_received_event);

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
  uint32_t error = NO_ERROR;

  if (!clipboard_rdp->server_file_contents_requests_allowed)
    {
      return send_file_contents_response_failure (cliprdr_context,
                                                  file_contents_request->streamId);
    }

  if (file_contents_request->dwFlags & FILECONTENTS_SIZE &&
      file_contents_request->dwFlags & FILECONTENTS_RANGE)
    {
      /**
       * FILECONTENTS_SIZE and FILECONTENTS_RANGE are not allowed
       * to be set at the same time
       */
      return send_file_contents_response_failure (cliprdr_context,
                                                  file_contents_request->streamId);
    }

  if (file_contents_request->dwFlags & FILECONTENTS_SIZE)
    {
      error = delegate_request_file_contents_size (clipboard_rdp->delegate,
                                                   file_contents_request);
    }
  else if (file_contents_request->dwFlags & FILECONTENTS_RANGE)
    {
      error = delegate_request_file_contents_range (clipboard_rdp->delegate,
                                                    file_contents_request);
    }
  else
    {
      error = ERROR_INVALID_DATA;
    }

  if (error)
    {
      return send_file_contents_response_failure (cliprdr_context,
                                                  file_contents_request->streamId);
    }

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

GrdClipboardRdp *
grd_clipboard_rdp_new (GrdSessionRdp *session_rdp,
                       HANDLE         vcm,
                       HANDLE         stop_event)
{
  GrdClipboardRdp *clipboard_rdp;
  GrdClipboard *clipboard;
  CliprdrServerContext *cliprdr_context;

  clipboard_rdp = g_object_new (GRD_TYPE_CLIPBOARD_RDP, NULL);
  cliprdr_context = cliprdr_server_context_new (vcm);
  if (!clipboard_rdp || !cliprdr_context)
    {
      g_warning ("[RDP.CLIPRDR] An error occurred while creating the RDP clipboard");
      g_clear_pointer (&cliprdr_context, cliprdr_server_context_free);
      g_clear_object (&clipboard_rdp);
      return NULL;
    }

  clipboard_rdp->cliprdr_context = cliprdr_context;
  clipboard_rdp->stop_event = stop_event;

  clipboard = GRD_CLIPBOARD (clipboard_rdp);
  grd_clipboard_initialize (clipboard, GRD_SESSION (session_rdp));

  cliprdr_context->useLongFormatNames = TRUE;
  cliprdr_context->streamFileClipEnabled = TRUE;
  cliprdr_context->fileClipNoFilePaths = TRUE;
  cliprdr_context->canLockClipData = FALSE;

  cliprdr_context->ClientCapabilities = cliprdr_client_capabilities;
  cliprdr_context->ClientFormatList = cliprdr_client_format_list;
  cliprdr_context->ClientFormatListResponse = cliprdr_client_format_list_response;
  cliprdr_context->ClientFormatDataRequest = cliprdr_client_format_data_request;
  cliprdr_context->ClientFormatDataResponse = cliprdr_client_format_data_response;
  cliprdr_context->ClientFileContentsRequest = cliprdr_client_file_contents_request;
  cliprdr_context->ClientFileContentsResponse = cliprdr_client_file_contents_response;
  cliprdr_context->custom = clipboard_rdp;

  if (cliprdr_context->Start (cliprdr_context))
    {
      g_message ("[RDP.CLIPRDR] An error occurred while starting the RDP "
                 "clipboard. The RDP client might not support the CLIPRDR channel");
      g_clear_pointer (&clipboard_rdp->cliprdr_context,
                       cliprdr_server_context_free);
      g_clear_object (&clipboard_rdp);
      return NULL;
    }

  return clipboard_rdp;
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

static void
grd_clipboard_rdp_dispose (GObject *object)
{
  GrdClipboardRdp *clipboard_rdp = GRD_CLIPBOARD_RDP (object);
  GrdClipboard *clipboard = GRD_CLIPBOARD (clipboard_rdp);

  if (clipboard_rdp->cliprdr_context)
    {
      clipboard_rdp->cliprdr_context->Stop (clipboard_rdp->cliprdr_context);
      grd_clipboard_disable_clipboard (clipboard);
    }

  if (clipboard_rdp->format_data_response)
    g_free ((uint8_t *) clipboard_rdp->format_data_response->requestedFormatData);
  g_clear_pointer (&clipboard_rdp->format_data_response, g_free);

  g_clear_pointer (&clipboard_rdp->queued_server_formats, g_list_free);
  g_clear_pointer (&clipboard_rdp->pending_server_formats, g_list_free);
  grd_rdp_fuse_clipboard_clear_selection (clipboard_rdp->rdp_fuse_clipboard);
  g_hash_table_foreach_remove (clipboard_rdp->format_data_cache,
                               clear_format_data,
                               NULL);

  g_clear_object (&clipboard_rdp->rdp_fuse_clipboard);
  rmdir (clipboard_rdp->fuse_mount_path);
  g_clear_pointer (&clipboard_rdp->fuse_mount_path, g_free);
  g_clear_pointer (&clipboard_rdp->format_data_cache, g_hash_table_unref);
  g_clear_pointer (&clipboard_rdp->allowed_server_formats, g_hash_table_destroy);
  g_clear_pointer (&clipboard_rdp->format_data_request_received_event,
                   CloseHandle);
  g_clear_pointer (&clipboard_rdp->format_list_response_received_event,
                   CloseHandle);
  g_clear_pointer (&clipboard_rdp->format_list_received_event, CloseHandle);
  g_clear_pointer (&clipboard_rdp->completed_format_data_request_event,
                   CloseHandle);
  g_clear_pointer (&clipboard_rdp->completed_format_list_event, CloseHandle);
  g_clear_pointer (&clipboard_rdp->format_data_received_event, CloseHandle);
  g_clear_pointer (&clipboard_rdp->system, ClipboardDestroy);
  g_clear_pointer (&clipboard_rdp->cliprdr_context, cliprdr_server_context_free);

  g_clear_handle_id (&clipboard_rdp->pending_server_formats_drop_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->server_format_list_update_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->server_format_data_request_id, g_source_remove);
  g_clear_handle_id (&clipboard_rdp->client_format_list_response_id, g_source_remove);

  G_OBJECT_CLASS (grd_clipboard_rdp_parent_class)->dispose (object);
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
          g_error ("Failed to create clipboard cache directory %s: %s",
                   base_path, g_strerror (errno));
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

  clipboard_rdp->format_data_received_event =
    CreateEvent (NULL, TRUE, FALSE, NULL);
  clipboard_rdp->completed_format_list_event =
    CreateEvent (NULL, TRUE, TRUE, NULL);
  clipboard_rdp->completed_format_data_request_event =
    CreateEvent (NULL, TRUE, TRUE, NULL);
  clipboard_rdp->format_list_received_event =
    CreateEvent (NULL, TRUE, FALSE, NULL);
  clipboard_rdp->format_list_response_received_event =
    CreateEvent (NULL, TRUE, FALSE, NULL);
  clipboard_rdp->format_data_request_received_event =
    CreateEvent (NULL, TRUE, FALSE, NULL);

  clipboard_rdp->allowed_server_formats = g_hash_table_new (NULL, NULL);
  clipboard_rdp->format_data_cache = g_hash_table_new (NULL, NULL);

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

  clipboard_class->update_client_mime_type_list =
    grd_clipboard_rdp_update_client_mime_type_list;
  clipboard_class->request_client_content_for_mime_type =
    grd_clipboard_rdp_request_client_content_for_mime_type;
}
