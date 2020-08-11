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

#ifndef GRD_MIME_TYPE_H
#define GRD_MIME_TYPE_H

#include <stdint.h>

typedef enum _GrdMimeType
{
  GRD_MIME_TYPE_NONE,
  GRD_MIME_TYPE_TEXT_PLAIN,            /* text/plain */
  GRD_MIME_TYPE_TEXT_PLAIN_UTF8,       /* text/plain;charset=utf-8 */
  GRD_MIME_TYPE_TEXT_UTF8_STRING,      /* UTF8_STRING */
  GRD_MIME_TYPE_TEXT_HTML,             /* text/html */
  GRD_MIME_TYPE_IMAGE_BMP,             /* image/bmp */
  GRD_MIME_TYPE_IMAGE_TIFF,            /* image/tiff */
  GRD_MIME_TYPE_IMAGE_GIF,             /* image/gif */
  GRD_MIME_TYPE_IMAGE_JPEG,            /* image/jpeg */
  GRD_MIME_TYPE_IMAGE_PNG,             /* image/png */
  GRD_MIME_TYPE_TEXT_URILIST,          /* text/uri-list */
  GRD_MIME_TYPE_XS_GNOME_COPIED_FILES, /* x-special/gnome-copied-files */
} GrdMimeType;

typedef struct _GrdMimeTypeTable
{
  GrdMimeType mime_type;

  struct
  {
    uint32_t format_id;
  } rdp;
} GrdMimeTypeTable;

const char *grd_mime_type_to_string (GrdMimeType mime_type);

GrdMimeType grd_mime_type_from_string (const char *mime_type_string);

#endif /* GRD_MIME_TYPE_H */
