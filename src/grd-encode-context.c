/*
 * Copyright (C) 2024 Pascal Nowack
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

#include "grd-encode-context.h"

#include <glib.h>

struct _GrdEncodeContext
{
  cairo_region_t *damage_region;
};

cairo_region_t *
grd_encode_context_get_damage_region (GrdEncodeContext *encode_context)
{
  return encode_context->damage_region;
}

void
grd_encode_context_set_damage_region (GrdEncodeContext *encode_context,
                                      cairo_region_t   *damage_region)
{
  encode_context->damage_region = cairo_region_reference (damage_region);
}

GrdEncodeContext *
grd_encode_context_new (void)
{
  return g_new0 (GrdEncodeContext, 1);
}

void
grd_encode_context_free (GrdEncodeContext *encode_context)
{
  g_clear_pointer (&encode_context->damage_region, cairo_region_destroy);

  g_free (encode_context);
}
