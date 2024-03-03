/*
 * Copyright (C) 2022 SUSE Software Solutions Germany GmbH
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
 *     Joan Torres <joan.torres@suse.com>
 */

#include "config.h"

#include <fcntl.h>
#include <gio/gio.h>

#include "grd-credentials-one-time.h"

struct _GrdCredentialsOneTime
{
  GrdCredentials parent;

  struct
  {
    char *username;
    char *password;
  } rdp;
};

G_DEFINE_TYPE (GrdCredentialsOneTime,
               grd_credentials_one_time,
               GRD_TYPE_CREDENTIALS)

static char *
grd_generate_random_bytes (size_t   size,
                           GError **error)
{
  int fd;
  int ret;
  char *bytes;

  errno = 0;
  fd = open ("/dev/urandom", O_RDONLY);
  if (fd < 0)
    {
      g_set_error_literal (error,
                           G_FILE_ERROR,
                           g_file_error_from_errno (errno),
                           g_strerror (errno));
      return NULL;
    }

  bytes = g_malloc (size);
  do
    ret = read (fd, bytes, size);
  while ((ret == -1 &&
         (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) ||
         ret < size);

  close (fd);

  return bytes;
}

static char *
generate_random_utf8_bytes (size_t   len,
                            GError **error)
{
  char *random_bytes;
  int i;

  random_bytes = grd_generate_random_bytes (len + 1, error);
  if (!random_bytes)
    return NULL;

  /* UTF-8 chars defined with 1 byte always have the MSB to 0.
   * Do not use ASCII control characters (0 - 32, 127). */
  for (i = 0; i < len; ++i)
    {
      random_bytes[i] &= 127;
      random_bytes[i] = (random_bytes[i] % 94) + 33;
    }
  random_bytes[len] = '\0';

  return random_bytes;
}

static char *
generate_random_username (size_t   len,
                          GError **error)
{
  char *username;

  username = generate_random_utf8_bytes (len, error);
  if (!username)
    return NULL;

  /* The use of # at the beggining or : at any position,
   * makes an error when looking up the user on the SAM file. */
  return g_strdelimit (username, "#:", '_');
}

static gboolean
grd_credentials_one_time_store (GrdCredentials      *credentials,
                                GrdCredentialsType   type,
                                GVariant            *variant,
                                GError             **error)
{
  GrdCredentialsOneTime *credentials_one_time =
    GRD_CREDENTIALS_ONE_TIME (credentials);
  g_autofree char *username = NULL;
  g_autofree char *password = NULL;

  g_assert (type == GRD_CREDENTIALS_TYPE_RDP);

  g_variant_lookup (variant, "username", "s", &username);
  if (!username)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Username not set");
      return FALSE;
    }

  g_variant_lookup (variant, "password", "s", &password);
  if (!password)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Password not set");
      return FALSE;
    }

  g_clear_pointer (&credentials_one_time->rdp.username, g_free);
  g_clear_pointer (&credentials_one_time->rdp.password, g_free);

  credentials_one_time->rdp.username = g_steal_pointer (&username);
  credentials_one_time->rdp.password = g_steal_pointer (&password);

  return TRUE;
}

static GVariant *
grd_credentials_one_time_lookup (GrdCredentials      *credentials,
                                 GrdCredentialsType   type,
                                 GError             **error)
{
  GrdCredentialsOneTime *credentials_one_time =
    GRD_CREDENTIALS_ONE_TIME (credentials);
  const char *rdp_username = credentials_one_time->rdp.username;
  const char *rdp_password = credentials_one_time->rdp.password;
  GVariantBuilder builder;

  switch (type)
    {
    case GRD_CREDENTIALS_TYPE_RDP:
      g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

      g_variant_builder_add (&builder, "{sv}",
                             "username", g_variant_new_string (rdp_username));
      g_variant_builder_add (&builder, "{sv}",
                             "password", g_variant_new_string (rdp_password));

      return g_variant_builder_end (&builder);
    default:
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                   "Credentials type not found");
      return NULL;
    }
}

GrdCredentialsOneTime *
grd_credentials_one_time_new (void)
{
  g_autoptr (GrdCredentialsOneTime) credentials_one_time = NULL;
  g_autoptr (GError) error = NULL;

  credentials_one_time = g_object_new (GRD_TYPE_CREDENTIALS_ONE_TIME, NULL);

  credentials_one_time->rdp.username = generate_random_username (16, &error);
  if (!credentials_one_time->rdp.username)
    {
      g_warning ("Failed to generate one time RDP username: %s", error->message);
      return NULL;
    }

  credentials_one_time->rdp.password = generate_random_utf8_bytes (16, &error);
  if (!credentials_one_time->rdp.password)
    {
      g_warning ("Failed to generate one time RDP password: %s", error->message);
      return NULL;
    }

  return g_steal_pointer (&credentials_one_time);
}

static void
grd_credentials_one_time_finalize (GObject *object)
{
  GrdCredentialsOneTime *credentials_one_time = GRD_CREDENTIALS_ONE_TIME (object);

  g_clear_pointer (&credentials_one_time->rdp.username, g_free);
  g_clear_pointer (&credentials_one_time->rdp.password, g_free);

  G_OBJECT_CLASS (grd_credentials_one_time_parent_class)->finalize (object);
}

static void
grd_credentials_one_time_init (GrdCredentialsOneTime *credentials_one_time)
{
}

static void
grd_credentials_one_time_class_init (GrdCredentialsOneTimeClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);
  GrdCredentialsClass *credentials_class = GRD_CREDENTIALS_CLASS (klass);

  object_class->finalize = grd_credentials_one_time_finalize;

  credentials_class->store = grd_credentials_one_time_store;
  credentials_class->lookup = grd_credentials_one_time_lookup;
}
