/*
 * Copyright (C) 2018 Red Hat Inc.
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

#include "grd-vnc-tls.h"

#include <errno.h>
#include <glib.h>
#include <gnutls/gnutls.h>
#include <rfb/rfb.h>

#include "grd-session-vnc.h"
#include "grd-vnc-server.h"

typedef struct _GrdVncTlsContext
{
  gnutls_anon_server_credentials_t anon_credentials;
  gnutls_dh_params_t dh_params;
} GrdVncTlsContext;

typedef enum _GrdTlsHandshakeState
{
  GRD_TLS_HANDSHAKE_STATE_INIT,
  GRD_TLS_HANDSHAKE_STATE_DURING,
  GRD_TLS_HANDSHAKE_STATE_FINISHED
} GrdTlsHandshakeState;

typedef struct _GrdVncTlsSession
{
  GrdVncTlsContext *tls_context;

  int fd;

  gnutls_session_t tls_session;
  GrdTlsHandshakeState handshake_state;

  char *peek_buffer;
  int peek_buffer_size;
  int peek_buffer_len;
} GrdVncTlsSession;

static gboolean
tls_handshake_grab_func (GrdSessionVnc  *session_vnc,
                         GError        **error);

static GrdVncTlsContext *
grd_vnc_tls_context_new (void)
{
  GrdVncTlsContext *tls_context;
  const unsigned int dh_bits = 1024;

  tls_context = g_new0 (GrdVncTlsContext, 1);

  rfbLog ("TLS: Initializing gnutls context\n");
  gnutls_global_init ();

  gnutls_anon_allocate_server_credentials (&tls_context->anon_credentials);

  gnutls_dh_params_init (&tls_context->dh_params);
  gnutls_dh_params_generate2 (tls_context->dh_params, dh_bits);

  gnutls_anon_set_server_dh_params (tls_context->anon_credentials,
                                    tls_context->dh_params);

  return tls_context;
}

static void
grd_vnc_tls_context_free (GrdVncTlsContext *tls_context)
{
  gnutls_dh_params_deinit (tls_context->dh_params);
  gnutls_anon_free_server_credentials (tls_context->anon_credentials);
  gnutls_global_deinit ();
}

GrdVncTlsContext *
ensure_tls_context (GrdVncServer *vnc_server)
{
  GrdVncTlsContext *tls_context;

  tls_context = g_object_get_data (G_OBJECT (vnc_server), "vnc-tls-context");
  if (!tls_context)
    {
      tls_context = grd_vnc_tls_context_new ();
      g_object_set_data_full (G_OBJECT (vnc_server), "vnc-tls-context",
                              tls_context,
                              (GDestroyNotify) grd_vnc_tls_context_free);
    }

  return tls_context;
}

static gboolean
perform_anon_tls_handshake (GrdVncTlsSession  *tls_session,
                            GError           **error)
{
  GrdVncTlsContext *tls_context = tls_session->tls_context;
  const char kx_priority[] = "NORMAL:+ANON-DH";
  int ret;

  gnutls_init (&tls_session->tls_session, GNUTLS_SERVER | GNUTLS_NO_SIGNAL);

  gnutls_set_default_priority (tls_session->tls_session);
  gnutls_priority_set_direct (tls_session->tls_session, kx_priority, NULL);

  gnutls_credentials_set (tls_session->tls_session,
                          GNUTLS_CRD_ANON,
                          tls_context->anon_credentials);
  gnutls_transport_set_ptr (tls_session->tls_session,
                            GINT_TO_POINTER (tls_session->fd));

  ret = gnutls_handshake (tls_session->tls_session);
  if (ret != GNUTLS_E_SUCCESS && !gnutls_error_is_fatal (ret))
    {
      rfbLog ("TLS: More handshake pending\n");
      tls_session->handshake_state = GRD_TLS_HANDSHAKE_STATE_DURING;
      return TRUE;
    }

  if (ret != GNUTLS_E_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", gnutls_strerror (ret));
      gnutls_deinit (tls_session->tls_session);
      tls_session->tls_session = NULL;
      return FALSE;
    }

  rfbLog ("TLS: Handshake finished");

  tls_session->handshake_state = GRD_TLS_HANDSHAKE_STATE_FINISHED;
  return TRUE;
}

static gboolean
continue_tls_handshake (GrdVncTlsSession  *tls_session,
                        GError           **error)
{
  int ret;

  ret = gnutls_handshake (tls_session->tls_session);
  if (ret != GNUTLS_E_SUCCESS && !gnutls_error_is_fatal (ret))
    return TRUE;

  if (ret != GNUTLS_E_SUCCESS)
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "%s", gnutls_strerror (ret));
      gnutls_deinit (tls_session->tls_session);
      tls_session->tls_session = NULL;
      return FALSE;
    }

  tls_session->handshake_state = GRD_TLS_HANDSHAKE_STATE_FINISHED;
  return TRUE;
}

static void
grd_vnc_tls_session_free (GrdVncTlsSession *tls_session)
{
  g_clear_pointer (&tls_session->peek_buffer, g_free);
  g_clear_pointer (&tls_session->tls_session, gnutls_deinit);
  g_free (tls_session);
}

static GrdVncTlsSession *
grd_vnc_tls_session_from_vnc_session (GrdSessionVnc *session_vnc)
{
  return g_object_get_data (G_OBJECT (session_vnc), "vnc-tls-session");
}

static int
do_read (GrdVncTlsSession *tls_session,
         char             *buf,
         int               len)
{
  do
    {
      int ret;

      ret = gnutls_record_recv (tls_session->tls_session, buf, len);
      if (ret == GNUTLS_E_AGAIN ||
          ret == GNUTLS_E_INTERRUPTED)
        {
          continue;
        }
      else if (ret < 0)
        {
          g_debug ("gnutls_record_recv failed: %s", gnutls_strerror (ret));
          errno = EIO;
          return -1;
        }
      else
        {
          return ret;
        }
    }
  while (TRUE);
}

static int
grd_vnc_tls_read_from_socket (rfbClientPtr  rfb_client,
                              char         *buf,
                              int           len)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncTlsSession *tls_session =
    grd_vnc_tls_session_from_vnc_session (session_vnc);
  int to_read = len;
  int len_read = 0;

  if (to_read < tls_session->peek_buffer_len)
    {
      memcpy (buf, tls_session->peek_buffer, to_read);
      memmove (buf,
               tls_session->peek_buffer + to_read,
               tls_session->peek_buffer_len - to_read);
      len_read = to_read;
      to_read = 0;
    }
  else
    {
      memcpy (buf,
              tls_session->peek_buffer,
              tls_session->peek_buffer_len);
      to_read -= tls_session->peek_buffer_len;
      len_read = tls_session->peek_buffer_len;

      g_clear_pointer (&tls_session->peek_buffer,
                       g_free);
      tls_session->peek_buffer_len = 0;
      tls_session->peek_buffer_size = 0;
    }

  if (to_read > 0)
    {
      int ret;

      ret = do_read (tls_session, buf + len_read, to_read);
      if (ret == -1)
        return -1;

      len_read += ret;
    }

  return len_read;
}

static int
grd_vnc_tls_peek_at_socket (rfbClientPtr  rfb_client,
                            char         *buf,
                            int           len)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncTlsSession *tls_session =
    grd_vnc_tls_session_from_vnc_session (session_vnc);
  int peekable_len;

  if (tls_session->peek_buffer_len < len)
    {
      int ret;

      if (len > tls_session->peek_buffer_size)
        {
          tls_session->peek_buffer = g_renew (char,
                                              tls_session->peek_buffer,
                                              len);
          tls_session->peek_buffer_size = len;
        }

      ret = do_read (tls_session,
                     tls_session->peek_buffer + tls_session->peek_buffer_len,
                     len - tls_session->peek_buffer_len);
      if (ret == -1)
        return -1;

      tls_session->peek_buffer_len += ret;
    }

  peekable_len = MIN (len, tls_session->peek_buffer_len);
  memcpy (buf, tls_session->peek_buffer, peekable_len);

  return peekable_len;
}

static rfbBool
grd_vnc_tls_has_pending_on_socket (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncTlsSession *tls_session =
    grd_vnc_tls_session_from_vnc_session (session_vnc);

  if (tls_session->peek_buffer_len > 0)
    return TRUE;

  if (gnutls_record_check_pending (tls_session->tls_session) > 0)
    return TRUE;

  return FALSE;
}

static int
grd_vnc_tls_write_to_socket (rfbClientPtr  rfb_client,
                             const char   *buf,
                             int           len)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncTlsSession *tls_session =
    grd_vnc_tls_session_from_vnc_session (session_vnc);

  do
    {
      int ret;

      ret = gnutls_record_send (tls_session->tls_session, buf, len);
      if (ret == GNUTLS_E_AGAIN ||
          ret == GNUTLS_E_INTERRUPTED)
        {
          continue;
        }
      else if (ret < 0)
        {
          g_debug ("gnutls_record_send failed: %s", gnutls_strerror (ret));
          errno = EIO;
          return -1;
        }
      else
        {
          return ret;
        }
    }
  while (TRUE);
}

static gboolean
perform_handshake (GrdSessionVnc  *session_vnc,
                   GError        **error)
{
  GrdVncTlsSession *tls_session =
    grd_vnc_tls_session_from_vnc_session (session_vnc);

  switch (tls_session->handshake_state)
    {
    case GRD_TLS_HANDSHAKE_STATE_INIT:
      if (!perform_anon_tls_handshake (tls_session, error))
        return FALSE;
      break;
    case GRD_TLS_HANDSHAKE_STATE_DURING:
      if (!continue_tls_handshake (tls_session, error))
        return FALSE;
      break;
    case GRD_TLS_HANDSHAKE_STATE_FINISHED:
      break;
    }

  switch (tls_session->handshake_state)
    {
    case GRD_TLS_HANDSHAKE_STATE_INIT:
      break;
    case GRD_TLS_HANDSHAKE_STATE_DURING:
      break;
    case GRD_TLS_HANDSHAKE_STATE_FINISHED:
      grd_session_vnc_ungrab_socket (session_vnc, tls_handshake_grab_func);
      rfbLog ("TLS: Sending post-channel security security list\n");
      rfbSendSecurityTypeList (grd_session_vnc_get_rfb_client (session_vnc),
                               RFB_SECURITY_TAG_CHANNEL);
      break;
    }

  return TRUE;
}

static gboolean
tls_handshake_grab_func (GrdSessionVnc  *session_vnc,
                         GError        **error)
{
  g_autoptr (GError) handshake_error = NULL;

  rfbLog ("TLS: Continuing handshake\n");
  if (!perform_handshake (session_vnc, &handshake_error))
    {
      g_set_error (error, G_IO_ERROR, G_IO_ERROR_FAILED,
                   "TLS handshake failed: %s", handshake_error->message);
      return FALSE;
    }

  return TRUE;
}

static void
rfb_tls_security_handler (rfbClientPtr rfb_client)
{
  GrdSessionVnc *session_vnc = rfb_client->screen->screenData;
  GrdVncTlsSession *tls_session;
  g_autoptr(GError) error = NULL;

  rfbLog ("TLS: Setting up rfbClient for gnutls encrypted traffic\n");

  tls_session = grd_vnc_tls_session_from_vnc_session (session_vnc);
  if (!tls_session)
    {
      GrdVncServer *vnc_server = grd_session_vnc_get_vnc_server (session_vnc);

      tls_session = g_new0 (GrdVncTlsSession, 1);
      tls_session->fd = grd_session_vnc_get_fd (session_vnc);
      tls_session->tls_context = ensure_tls_context (vnc_server);
      g_object_set_data_full (G_OBJECT (session_vnc), "vnc-tls-session",
                              tls_session,
                              (GDestroyNotify) grd_vnc_tls_session_free);

      rfb_client->readFromSocket = grd_vnc_tls_read_from_socket;
      rfb_client->peekAtSocket = grd_vnc_tls_peek_at_socket;
      rfb_client->hasPendingOnSocket = grd_vnc_tls_has_pending_on_socket;
      rfb_client->writeToSocket = grd_vnc_tls_write_to_socket;

      grd_session_vnc_grab_socket (session_vnc, tls_handshake_grab_func);
    }

  rfbLog ("TLS: Performing handshake\n");
  if (!perform_handshake (session_vnc, &error))
    {
      g_warning ("TLS handshake failed: %s", error->message);
      rfbCloseClient (rfb_client);
    }
}

static rfbSecurityHandler anon_tls_security_handler = {
  .type = rfbTLS,
  .handler = rfb_tls_security_handler,
  .securityTags = RFB_SECURITY_TAG_CHANNEL,
};

rfbSecurityHandler *
grd_vnc_tls_get_security_handler (void)
{
  return &anon_tls_security_handler;
}
