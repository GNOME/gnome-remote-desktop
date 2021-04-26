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
  const char *path;
  char *filename;
  char *sam_string;
  int fd_flags;
  GrdRdpSAMFile *rdp_sam_file;
  FILE *sam_file;

  rdp_sam_file = g_malloc0 (sizeof (GrdRdpSAMFile));

  path = getenv ("XDG_RUNTIME_DIR");
  filename = g_malloc0 (strlen (path) + strlen (grd_path) +
                        strlen (template) + 1);
  strcpy (filename, path);
  strcat (filename, grd_path);
  if (g_access (filename, F_OK))
    mkdir (filename, 0700);

  strcat (filename, template);

  rdp_sam_file->fd = mkstemp (filename);
  rdp_sam_file->filename = filename;

  fd_flags = fcntl (rdp_sam_file->fd, F_GETFD);
  fcntl (rdp_sam_file->fd, F_SETFD, fd_flags | FD_CLOEXEC);

  sam_string = create_sam_string (username, password);

  sam_file = fdopen (rdp_sam_file->fd, "w+");
  fputs (sam_string, sam_file);
  fclose (sam_file);

  g_free (sam_string);

  return rdp_sam_file;
}

void
grd_rdp_sam_maybe_close_and_free_sam_file (GrdRdpSAMFile *rdp_sam_file)
{
  if (rdp_sam_file->fd >= 0)
    {
      unlink (rdp_sam_file->filename);
      close (rdp_sam_file->fd);
    }

  g_free (rdp_sam_file->filename);
  g_free (rdp_sam_file);
}
