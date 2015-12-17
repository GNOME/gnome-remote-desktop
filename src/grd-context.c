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

#include "config.h"

#include "grd-context.h"

#include "grd-dbus-remote-desktop.h"

struct _GrdContext
{
  GObject parent;

  GrdDBusRemoteDesktop *proxy;
};

G_DEFINE_TYPE (GrdContext, grd_context, G_TYPE_OBJECT);

GrdDBusRemoteDesktop *
grd_context_get_dbus_proxy (GrdContext *context)
{
  return context->proxy;
}

void
grd_context_set_dbus_proxy (GrdContext           *context,
                            GrdDBusRemoteDesktop *proxy)
{
  context->proxy = proxy;
}

static void
grd_context_init (GrdContext *context)
{
}

static void
grd_context_class_init (GrdContextClass *klass)
{
}
