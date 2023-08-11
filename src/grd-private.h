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

#ifndef GRD_PRIVATE_H
#define GRD_PRIVATE_H

#define GRD_DAEMON_USER_APPLICATION_ID "org.gnome.RemoteDesktop.User"
#define GRD_DAEMON_SYSTEM_APPLICATION_ID "org.gnome.RemoteDesktop.System"
#define REMOTE_DESKTOP_BUS_NAME "org.gnome.RemoteDesktop"
#define REMOTE_DESKTOP_OBJECT_PATH "/org/gnome/RemoteDesktop"
#define REMOTE_DESKTOP_HANDOVER_OBJECT_PATH "/org/gnome/RemoteDesktop/Handover"
#define REMOTE_DESKTOP_CLIENT_OBJECT_PATH "/org/gnome/RemoteDesktop/Client"
#define MUTTER_REMOTE_DESKTOP_BUS_NAME "org.gnome.Mutter.RemoteDesktop"
#define MUTTER_REMOTE_DESKTOP_OBJECT_PATH "/org/gnome/Mutter/RemoteDesktop"
#define MUTTER_SCREEN_CAST_BUS_NAME "org.gnome.Mutter.ScreenCast"
#define MUTTER_SCREEN_CAST_OBJECT_PATH "/org/gnome/Mutter/ScreenCast"
#define GDM_BUS_NAME "org.gnome.DisplayManager"
#define GDM_REMOTE_DISPLAY_FACTORY_OBJECT_PATH "/org/gnome/DisplayManager/RemoteDisplayFactory"
#define GDM_OBJECT_MANAGER_OBJECT_PATH "/org/gnome/DisplayManager/Displays"

#endif /* GRD_PRIVATE_H */
