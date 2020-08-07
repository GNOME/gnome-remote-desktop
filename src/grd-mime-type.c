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

#include "grd-mime-type.h"

#include <gio/gio.h>

const char *
grd_mime_type_to_string (GrdMimeType mime_type)
{
  switch (mime_type)
    {
    case GRD_MIME_TYPE_TEXT_PLAIN:
      return "text/plain";
    case GRD_MIME_TYPE_TEXT_PLAIN_UTF8:
      return "text/plain;charset=utf-8";
    case GRD_MIME_TYPE_TEXT_UTF8_STRING:
      return "UTF8_STRING";
    case GRD_MIME_TYPE_TEXT_HTML:
      return "text/html";
    case GRD_MIME_TYPE_IMAGE_BMP:
      return "image/bmp";
    case GRD_MIME_TYPE_IMAGE_TIFF:
      return "image/tiff";
    case GRD_MIME_TYPE_IMAGE_GIF:
      return "image/gif";
    case GRD_MIME_TYPE_IMAGE_JPEG:
      return "image/jpeg";
    case GRD_MIME_TYPE_IMAGE_PNG:
      return "image/png";
    case GRD_MIME_TYPE_TEXT_URILIST:
      return "text/uri-list";
    case GRD_MIME_TYPE_XS_GNOME_COPIED_FILES:
      return "x-special/gnome-copied-files";
    default:
      return NULL;
    }

  g_assert_not_reached ();
}

GrdMimeType
grd_mime_type_from_string (const char *mime_type_string)
{
  if (strcmp (mime_type_string, "text/plain") == 0)
    return GRD_MIME_TYPE_TEXT_PLAIN;
  else if (strcmp (mime_type_string, "text/plain;charset=utf-8") == 0)
    return GRD_MIME_TYPE_TEXT_PLAIN_UTF8;
  else if (strcmp (mime_type_string, "UTF8_STRING") == 0)
    return GRD_MIME_TYPE_TEXT_UTF8_STRING;
  else if (strcmp (mime_type_string, "text/html") == 0)
    return GRD_MIME_TYPE_TEXT_HTML;
  else if (strcmp (mime_type_string, "image/bmp") == 0)
    return GRD_MIME_TYPE_IMAGE_BMP;
  else if (strcmp (mime_type_string, "image/tiff") == 0)
    return GRD_MIME_TYPE_IMAGE_TIFF;
  else if (strcmp (mime_type_string, "image/gif") == 0)
    return GRD_MIME_TYPE_IMAGE_GIF;
  else if (strcmp (mime_type_string, "image/jpeg") == 0)
    return GRD_MIME_TYPE_IMAGE_JPEG;
  else if (strcmp (mime_type_string, "image/png") == 0)
    return GRD_MIME_TYPE_IMAGE_PNG;
  else if (strcmp (mime_type_string, "text/uri-list") == 0)
    return GRD_MIME_TYPE_TEXT_URILIST;
  else if (strcmp (mime_type_string, "x-special/gnome-copied-files") == 0)
    return GRD_MIME_TYPE_XS_GNOME_COPIED_FILES;

  return GRD_MIME_TYPE_NONE;
}
