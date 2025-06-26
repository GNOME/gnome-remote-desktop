/*
 * Copyright (C) 2022 Pascal Nowack
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

#define G_LOG_DOMAIN "grd-test-client-rdp"

#include <freerdp/client/cmdline.h>
#include <freerdp/freerdp.h>
#include <freerdp/gdi/gfx.h>
#include <glib.h>

#define TEST_1_WIDTH 800
#define TEST_1_HEIGHT 600

typedef enum _TestState
{
  TEST_STATE_TEST_1,
  TEST_STATE_TESTS_COMPLETE,
} TestState;

typedef struct _RdpPeerContext
{
  rdpContext rdp_context;

  HANDLE stop_event;

  TestState test_state;
} RdpPeerContext;

static const char *
test_state_to_test_string (TestState test_state)
{
  switch (test_state)
    {
    case TEST_STATE_TEST_1:
      return "1 (Virtual monitor size)";
    case TEST_STATE_TESTS_COMPLETE:
      g_assert_not_reached ();
    }

  g_assert_not_reached ();
}

static void
transition_test_state (RdpPeerContext *rdp_peer_context)
{
  g_message ("Test %s was successful!",
             test_state_to_test_string (rdp_peer_context->test_state));

  switch (rdp_peer_context->test_state)
    {
    case TEST_STATE_TEST_1:
      rdp_peer_context->test_state = TEST_STATE_TESTS_COMPLETE;
      break;
    case TEST_STATE_TESTS_COMPLETE:
      break;
    }

  if (rdp_peer_context->test_state == TEST_STATE_TESTS_COMPLETE)
    freerdp_client_stop (&rdp_peer_context->rdp_context);
}

static void
exit_test_failure (RdpPeerContext *rdp_peer_context)
{
  g_warning ("Test %s failed!",
             test_state_to_test_string (rdp_peer_context->test_state));
  freerdp_client_stop (&rdp_peer_context->rdp_context);
}

static void
handle_test_result (RdpPeerContext *rdp_peer_context,
                    gboolean        success)
{
  if (success)
    transition_test_state (rdp_peer_context);
  else
    exit_test_failure (rdp_peer_context);
}

static void
init_graphics_pipeline (RdpPeerContext      *rdp_peer_context,
                        RdpgfxClientContext *rdpgfx_context)
{
  rdpContext *rdp_context = &rdp_peer_context->rdp_context;

  gdi_graphics_pipeline_init (rdp_context->gdi, rdpgfx_context);
}

static void
uninit_graphics_pipeline (RdpPeerContext      *rdp_peer_context,
                          RdpgfxClientContext *rdpgfx_context)
{
  rdpContext *rdp_context = &rdp_peer_context->rdp_context;

  gdi_graphics_pipeline_uninit (rdp_context->gdi, rdpgfx_context);
}

static void
on_channel_connected (void                            *user_data,
                      const ChannelConnectedEventArgs *event_args)
{
  if (strcmp (event_args->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
    init_graphics_pipeline (user_data, event_args->pInterface);
}

static void
on_channel_disconnected (void                               *user_data,
                         const ChannelDisconnectedEventArgs *event_args)
{
  if (strcmp (event_args->name, RDPGFX_DVC_CHANNEL_NAME) == 0)
    uninit_graphics_pipeline (user_data, event_args->pInterface);
}

static BOOL
rdp_client_pre_connect (freerdp *instance)
{
  rdpContext *rdp_context = instance->context;
  rdpSettings *rdp_settings = rdp_context->settings;
  rdpChannels *rdp_channels = rdp_context->channels;

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_OsMajorType,
                               OSMAJORTYPE_UNIX);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_OsMinorType,
                               OSMINORTYPE_PSEUDO_XSERVER);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_SupportGraphicsPipeline, TRUE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_SupportDisplayControl, FALSE);
  freerdp_settings_set_bool (rdp_settings, FreeRDP_NetworkAutoDetect, TRUE);

  freerdp_settings_set_bool (rdp_settings, FreeRDP_RedirectClipboard, FALSE);

  PubSub_SubscribeChannelConnected (rdp_context->pubSub,
                                    on_channel_connected);
  PubSub_SubscribeChannelDisconnected (rdp_context->pubSub,
                                       on_channel_disconnected);

  if (!freerdp_client_load_addins (rdp_channels, rdp_settings))
    return FALSE;

  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopWidth,
                               TEST_1_WIDTH);
  freerdp_settings_set_uint32 (rdp_settings, FreeRDP_DesktopHeight,
                               TEST_1_HEIGHT);

  return TRUE;
}

static BOOL
rdp_update_desktop_resize (rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  rdpSettings *rdp_settings = rdp_context->settings;
  uint32_t desktop_width =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_DesktopWidth);
  uint32_t desktop_height =
    freerdp_settings_get_uint32 (rdp_settings, FreeRDP_DesktopHeight);

  g_message ("New desktop size: [%u, %u]",
             desktop_width, desktop_height);

  if (!gdi_resize (rdp_context->gdi, desktop_width, desktop_height))
    return FALSE;

  if (rdp_peer_context->test_state == TEST_STATE_TEST_1 &&
      (desktop_width != TEST_1_WIDTH ||
       desktop_height != TEST_1_HEIGHT))
    exit_test_failure (rdp_peer_context);

  return TRUE;
}

static BOOL
rdp_update_end_paint (rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  int32_t x, y;
  uint32_t width, height;

  x = rdp_context->gdi->primary->hdc->hwnd->invalid->x;
  y = rdp_context->gdi->primary->hdc->hwnd->invalid->y;
  width = rdp_context->gdi->primary->hdc->hwnd->invalid->w;
  height = rdp_context->gdi->primary->hdc->hwnd->invalid->h;

  g_message ("Region invalidated: [x: %i, y: %i, w: %u, h: %u]",
             x, y, width, height);

  if (rdp_peer_context->test_state == TEST_STATE_TEST_1)
    {
      gboolean success;

      success = x == 0 &&
                y == 0 &&
                width == TEST_1_WIDTH &&
                height == TEST_1_HEIGHT;

      handle_test_result (rdp_peer_context, success);
    }

  return TRUE;
}

static BOOL
rdp_client_post_connect (freerdp *instance)
{
  if (!gdi_init (instance, PIXEL_FORMAT_BGRX32))
    return FALSE;

  instance->context->update->DesktopResize = rdp_update_desktop_resize;
  instance->context->update->EndPaint = rdp_update_end_paint;

  return TRUE;
}

static void
rdp_client_post_disconnect (freerdp *instance)
{
  rdpContext *rdp_context = instance->context;

  gdi_free (instance);

  PubSub_UnsubscribeChannelDisconnected (rdp_context->pubSub,
                                         on_channel_disconnected);
  PubSub_UnsubscribeChannelConnected (rdp_context->pubSub,
                                      on_channel_connected);
}

static BOOL
rdp_client_authenticate (freerdp  *instance,
                         char    **username,
                         char    **password,
                         char    **domain)
{
  /* Credentials were already parsed from the command line */

  return TRUE;
}

static uint32_t
rdp_client_verify_certificate_ex (freerdp    *instance,
                                  const char *host,
                                  uint16_t    port,
                                  const char *common_name,
                                  const char *subject,
                                  const char *issuer,
                                  const char *fingerprint,
                                  uint32_t    flags)
{
  /* For this test client, always accept any certificate */

  return 2;
}

static uint32_t
rdp_client_verify_changed_certificate_ex (freerdp    *instance,
                                          const char *host,
                                          uint16_t    port,
                                          const char *common_name,
                                          const char *subject,
                                          const char *issuer,
                                          const char *fingerprint,
                                          const char *old_subject,
                                          const char *old_issuer,
                                          const char *old_fingerprint,
                                          uint32_t    flags)
{
  return 2;
}

static void
on_terminate (void                     *user_data,
              const TerminateEventArgs *event_args)
{
  rdpContext *rdp_context = user_data;

  freerdp_abort_connect_context (rdp_context);
}

static BOOL
rdp_client_new (freerdp    *instance,
                rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;

  g_message ("Creating client");

  instance->PreConnect = rdp_client_pre_connect;
  instance->PostConnect = rdp_client_post_connect;
  instance->PostDisconnect = rdp_client_post_disconnect;
  instance->Authenticate = rdp_client_authenticate;
  instance->VerifyCertificateEx = rdp_client_verify_certificate_ex;
  instance->VerifyChangedCertificateEx = rdp_client_verify_changed_certificate_ex;

  PubSub_SubscribeTerminate (rdp_context->pubSub, on_terminate);

  rdp_peer_context->stop_event = CreateEvent (NULL, TRUE, FALSE, NULL);
  if (!rdp_peer_context->stop_event)
    return FALSE;

  return TRUE;
}

static void
rdp_client_free (freerdp    *instance,
                 rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;

  g_message ("Freeing client");

  g_clear_pointer (&rdp_peer_context->stop_event, CloseHandle);

  PubSub_UnsubscribeTerminate (rdp_context->pubSub, on_terminate);
}

static int
rdp_client_start (rdpContext *rdp_context)
{
  rdpSettings *rdp_settings = rdp_context->settings;

  if (!freerdp_settings_get_string (rdp_settings, FreeRDP_ServerHostname))
    return -1;

  return 0;
}

static int
rdp_client_stop (rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;

  SetEvent (rdp_peer_context->stop_event);

  return 0;
}

static gboolean
run_main_loop (rdpContext *rdp_context)
{
  RdpPeerContext *rdp_peer_context = (RdpPeerContext *) rdp_context;
  HANDLE events[64];
  uint32_t n_events;
  uint32_t n_freerdp_handles;
  gboolean result;

  rdp_peer_context->test_state = TEST_STATE_TEST_1;

  if (!freerdp_connect (rdp_context->instance))
    {
      g_warning ("Failed to connect to server");
      return FALSE;
    }

  while (!freerdp_shall_disconnect_context (rdp_context))
    {
      n_events = 0;

      events[n_events++] = rdp_peer_context->stop_event;

      n_freerdp_handles = freerdp_get_event_handles (rdp_context,
                                                     &events[n_events],
                                                     64 - n_events);
      if (!n_freerdp_handles)
        {
          g_warning ("Failed to get FreeRDP event handles");
          return FALSE;
        }
      n_events += n_freerdp_handles;

      WaitForMultipleObjects (n_events, events, FALSE, INFINITE);

      if (WaitForSingleObject (rdp_peer_context->stop_event, 0) == WAIT_OBJECT_0)
        break;

      if (!freerdp_check_event_handles (rdp_context))
        {
          if (client_auto_reconnect_ex (rdp_context->instance, NULL))
            continue;

          if (freerdp_get_last_error (rdp_context) == FREERDP_ERROR_SUCCESS)
            {
              g_warning ("Failed to check FreeRDP file descriptor");
              break;
            }
        }
    }

  result = rdp_peer_context->test_state == TEST_STATE_TESTS_COMPLETE;
  g_debug ("Main loop tests complete: %i", result);

  return result;
}

int
main (int    argc,
      char **argv)
{
  RDP_CLIENT_ENTRY_POINTS client_entry_points = {};
  rdpContext *rdp_context;
  gboolean success;

  client_entry_points.Version = 1;
  client_entry_points.Size = sizeof (RDP_CLIENT_ENTRY_POINTS_V1);
  client_entry_points.ContextSize = sizeof (RdpPeerContext);
  client_entry_points.ClientNew = rdp_client_new;
  client_entry_points.ClientFree = rdp_client_free;
  client_entry_points.ClientStart = rdp_client_start;
  client_entry_points.ClientStop = rdp_client_stop;

  rdp_context = freerdp_client_context_new (&client_entry_points);
  if (!rdp_context)
    g_error ("Failed to create client context");

  if (freerdp_client_settings_parse_command_line (rdp_context->settings,
                                                  argc, argv, FALSE))
    {
      g_warning ("Failed to parse command line");
      freerdp_client_context_free (rdp_context);
      return 1;
    }
  if (freerdp_client_start (rdp_context))
    {
      g_warning ("Failed to start client");
      freerdp_client_context_free (rdp_context);
      return 1;
    }

  success = run_main_loop (rdp_context);

  freerdp_client_context_free (rdp_context);

  g_debug ("Client context freed. Exiting test with result: %i", success);

  if (success)
    return 0;

  return 1;
}
