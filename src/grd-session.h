/*
 * Copyright (C) 2015 Red Hat Inc.
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
 * Written by:
 *     Jonas Ådahl <jadahl@gmail.com>
 */

#ifndef GRD_SESSION_H
#define GRD_SESSION_H

#include <glib-object.h>

#include "grd-types.h"

#define GRD_TYPE_SESSION (grd_session_get_type ())
G_DECLARE_DERIVABLE_TYPE (GrdSession, grd_session, GRD, SESSION, GObject);

struct _GrdSessionClass
{
  GObjectClass parent_class;

  void (*stream_added) (GrdSession *session,
                        GrdStream  *stream);
  void (*stop) (GrdSession *session);
};

GrdContext *grd_session_get_context (GrdSession *session);

void grd_session_stop (GrdSession *session);

#endif /* GRD_SESSION_H */
