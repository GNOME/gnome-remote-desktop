/*
 * Copyright (C) 2023 SUSE Software Solutions Germany GmbH
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

#ifndef GRD_DAEMON_UTILS_H
#define GRD_DAEMON_UTILS_H

#include <gio/gio.h>

gboolean grd_get_pid_of_sender_sync (GDBusConnection  *connection,
                                     const char       *name,
                                     pid_t            *out_pid,
                                     GCancellable     *cancellable,
                                     GError          **error);

gboolean grd_get_uid_of_sender_sync (GDBusConnection  *connection,
                                     const char       *name,
                                     uid_t            *out_uid,
                                     GCancellable     *cancellable,
                                     GError          **error);

char *grd_get_session_id_from_pid (pid_t pid);

char *grd_get_session_id_from_uid (uid_t uid);

#endif /* GRD_DAEMON_UTILS_H */
