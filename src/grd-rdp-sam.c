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

#include "grd-rdp-sam.h"

#include <fcntl.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <sys/stat.h>
#include <winpr/ntlm.h>

void
grd_rdp_sam_free_sam_file (GrdRdpSAMFile *rdp_sam_file)
{
  if (rdp_sam_file->fd >= 0)
    {
      unlink (rdp_sam_file->filename);
      close (rdp_sam_file->fd);
    }

  g_free (rdp_sam_file->filename);
  g_free (rdp_sam_file);
}

static char *
create_sam_string (const char *username,
                   const char *password)
{
  uint32_t username_length;
  uint32_t password_length;
  uint32_t i;
  char *sam_string;
  uint8_t nt_hash[16];

  username_length = strlen (username);
  password_length = strlen (password);

  sam_string = g_malloc0 ((username_length + 3 + 32 + 3 + 1 + 1) * sizeof (char));

  NTOWFv1A ((LPSTR) password, password_length, nt_hash);

  sprintf (sam_string, "%s:::", username);
  for (i = 0; i < 16; ++i)
    sprintf (sam_string + strlen (sam_string), "%02" PRIx8 "", nt_hash[i]);
  sprintf (sam_string + strlen (sam_string), ":::\n");

  return sam_string;
}

GrdRdpSAMFile *
grd_rdp_sam_create_sam_file (const char *username,
                             const char *password)
{
  const char *grd_path = "/gnome-remote-desktop";
  const char *template = "/rdp-sam-XXXXXX";
  int duped_fd;
  GrdRdpSAMFile *rdp_sam_file;
  g_autofree char *file_dir = NULL;
  g_autofree char *filename = NULL;
  g_autofree char *sam_string = NULL;
  g_autofd int fd = -1;
  FILE *sam_file;

  file_dir = g_strdup_printf ("%s%s", g_get_user_runtime_dir (), grd_path);
  filename = g_strdup_printf ("%s%s%s", g_get_user_runtime_dir (), grd_path,
                              template);

  if (g_access (file_dir, F_OK) &&
      mkdir (file_dir, 0700))
    {
      g_warning ("Failed to create base runtime directory for "
                 "gnome-remote-desktop: %s", g_strerror (errno));
      return NULL;
    }

  fd = g_mkstemp (filename);
  if (fd < 0)
    {
      g_warning ("[RDP] g_mkstemp() failed: %s", g_strerror (errno));
      return NULL;
    }

  sam_file = fdopen (fd, "w+");
  if (!sam_file)
    {
      g_warning ("[RDP] Failed to open SAM database: %s", g_strerror (errno));
      return NULL;
    }

  duped_fd = dup (fd);
  if (duped_fd < 0)
    {
      fclose (sam_file);
      g_warning ("[RDP] Failed to dup fd: %s", g_strerror (errno));
      return NULL;
    }

  rdp_sam_file = g_new0 (GrdRdpSAMFile, 1);
  rdp_sam_file->fd = duped_fd;
  rdp_sam_file->filename = g_steal_pointer (&filename);

  g_steal_fd (&fd);

  sam_string = create_sam_string (username, password);

  fputs (sam_string, sam_file);
  fclose (sam_file);

  return rdp_sam_file;
}
