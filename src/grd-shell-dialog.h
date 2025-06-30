/*
 * Copyright (C) 2024 SUSE Software Solutions Germany GmbH
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
 */

#pragma once

#include <gio/gio.h>
#include <glib-object.h>

#define GRD_TYPE_SHELL_DIALOG (grd_shell_dialog_get_type ())
G_DECLARE_FINAL_TYPE (GrdShellDialog, grd_shell_dialog,
                      GRD, SHELL_DIALOG, GObject)

GrdShellDialog *grd_shell_dialog_new (GCancellable *cancellable);

void grd_shell_dialog_open (GrdShellDialog *shell_dialog,
                            const char     *title,
                            const char     *description,
                            const char     *cancel_label,
                            const char     *accept_label);
