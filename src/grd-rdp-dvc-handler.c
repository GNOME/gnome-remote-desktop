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

#include "grd-rdp-dvc-handler.h"

typedef struct _DVCSubscription
{
  gboolean notified;

  GrdRdpDVCCreationStatusCallback callback;
  gpointer user_data;
} DVCSubscription;

typedef struct _DVCNotification
{
  int32_t creation_status;
  gboolean pending_status;

  GHashTable *subscriptions;
  uint32_t next_subscription_id;
} DVCNotification;

struct _GrdRdpDvcHandler
{
  GObject parent;

  GMutex dvc_notification_mutex;
  GHashTable *dvc_table;
  GSource *dvc_notification_source;
};

G_DEFINE_TYPE (GrdRdpDvcHandler, grd_rdp_dvc_handler,
               G_TYPE_OBJECT)

static DVCNotification *
dvc_notification_new (void)
{
  DVCNotification *dvc_notification;

  dvc_notification = g_new0 (DVCNotification, 1);
  dvc_notification->pending_status = TRUE;
  dvc_notification->subscriptions = g_hash_table_new_full (NULL, NULL,
                                                           NULL, g_free);

  return dvc_notification;
}

static uint32_t
get_next_free_dvc_subscription_id (DVCNotification *dvc_notification)
{
  uint32_t subscription_id = dvc_notification->next_subscription_id;

  while (g_hash_table_contains (dvc_notification->subscriptions,
                                GUINT_TO_POINTER (subscription_id)))
    ++subscription_id;

  dvc_notification->next_subscription_id = subscription_id + 1;

  return subscription_id;
}

static uint32_t
dvc_notification_add_subscription (DVCNotification *dvc_notification,
                                   DVCSubscription *dvc_subscription)
{
  uint32_t subscription_id;

  subscription_id = get_next_free_dvc_subscription_id (dvc_notification);
  g_hash_table_insert (dvc_notification->subscriptions,
                       GUINT_TO_POINTER (subscription_id), dvc_subscription);

  return subscription_id;
}

uint32_t
grd_rdp_dvc_handler_subscribe_dvc_creation_status (GrdRdpDvcHandler                *dvc_handler,
                                                   uint32_t                         channel_id,
                                                   GrdRdpDVCCreationStatusCallback  callback,
                                                   gpointer                         callback_user_data)
{
  DVCNotification *dvc_notification;
  g_autofree DVCSubscription *dvc_subscription = NULL;
  uint32_t subscription_id;
  gboolean pending_notification = FALSE;

  dvc_subscription = g_new0 (DVCSubscription, 1);
  dvc_subscription->callback = callback;
  dvc_subscription->user_data = callback_user_data;

  g_mutex_lock (&dvc_handler->dvc_notification_mutex);
  if (g_hash_table_lookup_extended (dvc_handler->dvc_table,
                                    GUINT_TO_POINTER (channel_id),
                                    NULL, (gpointer *) &dvc_notification))
    {
      subscription_id =
        dvc_notification_add_subscription (dvc_notification,
                                           g_steal_pointer (&dvc_subscription));

      if (!dvc_notification->pending_status)
        pending_notification = TRUE;
    }
  else
    {
      dvc_notification = dvc_notification_new ();

      subscription_id =
        dvc_notification_add_subscription (dvc_notification,
                                           g_steal_pointer (&dvc_subscription));

      g_hash_table_insert (dvc_handler->dvc_table,
                           GUINT_TO_POINTER (channel_id), dvc_notification);
    }
  g_mutex_unlock (&dvc_handler->dvc_notification_mutex);

  if (pending_notification)
    g_source_set_ready_time (dvc_handler->dvc_notification_source, 0);

  return subscription_id;
}

void
grd_rdp_dvc_handler_unsubscribe_dvc_creation_status (GrdRdpDvcHandler *dvc_handler,
                                                     uint32_t          channel_id,
                                                     uint32_t          subscription_id)
{
  DVCNotification *dvc_notification;

  g_mutex_lock (&dvc_handler->dvc_notification_mutex);
  if (!g_hash_table_lookup_extended (dvc_handler->dvc_table,
                                     GUINT_TO_POINTER (channel_id),
                                     NULL, (gpointer *) &dvc_notification))
    g_assert_not_reached ();

  g_hash_table_remove (dvc_notification->subscriptions,
                       GUINT_TO_POINTER (subscription_id));
  g_mutex_unlock (&dvc_handler->dvc_notification_mutex);
}

static BOOL
dvc_creation_status (void     *user_data,
                     uint32_t  channel_id,
                     int32_t   creation_status)
{
  GrdRdpDvcHandler *dvc_handler = user_data;
  DVCNotification *dvc_notification;
  gboolean pending_notification = FALSE;

  g_debug ("[RDP.DRDYNVC] DVC channel id %u creation status: %i",
           channel_id, creation_status);

  g_mutex_lock (&dvc_handler->dvc_notification_mutex);
  if (g_hash_table_lookup_extended (dvc_handler->dvc_table,
                                    GUINT_TO_POINTER (channel_id),
                                    NULL, (gpointer *) &dvc_notification))
    {
      if (dvc_notification->pending_status)
        {
          dvc_notification->creation_status = creation_status;
          dvc_notification->pending_status = FALSE;

          if (g_hash_table_size (dvc_notification->subscriptions) > 0)
            pending_notification = TRUE;
        }
      else
        {
          g_warning ("[RDP.DRDYNVC] Status of channel %u already known. "
                     "Discarding result", channel_id);
        }
    }
  else
    {
      dvc_notification = dvc_notification_new ();

      dvc_notification->creation_status = creation_status;
      dvc_notification->pending_status = FALSE;

      g_hash_table_insert (dvc_handler->dvc_table,
                           GUINT_TO_POINTER (channel_id), dvc_notification);
    }
  g_mutex_unlock (&dvc_handler->dvc_notification_mutex);

  if (pending_notification)
    g_source_set_ready_time (dvc_handler->dvc_notification_source, 0);

  return TRUE;
}

GrdRdpDvcHandler *
grd_rdp_dvc_handler_new (HANDLE vcm)
{
  GrdRdpDvcHandler *dvc_handler;

  dvc_handler = g_object_new (GRD_TYPE_RDP_DVC_HANDLER, NULL);

  WTSVirtualChannelManagerSetDVCCreationCallback (vcm, dvc_creation_status,
                                                  dvc_handler);

  return dvc_handler;
}

static void
grd_rdp_dvc_handler_dispose (GObject *object)
{
  GrdRdpDvcHandler *dvc_handler = GRD_RDP_DVC_HANDLER (object);

  if (dvc_handler->dvc_notification_source)
    {
      g_source_destroy (dvc_handler->dvc_notification_source);
      g_clear_pointer (&dvc_handler->dvc_notification_source, g_source_unref);
    }

  g_clear_pointer (&dvc_handler->dvc_table, g_hash_table_unref);

  G_OBJECT_CLASS (grd_rdp_dvc_handler_parent_class)->dispose (object);
}

static void
grd_rdp_dvc_handler_finalize (GObject *object)
{
  GrdRdpDvcHandler *dvc_handler = GRD_RDP_DVC_HANDLER (object);

  g_mutex_clear (&dvc_handler->dvc_notification_mutex);

  G_OBJECT_CLASS (grd_rdp_dvc_handler_parent_class)->finalize (object);
}

static void
dvc_notification_free (gpointer data)
{
  DVCNotification *dvc_notification = data;

  g_clear_pointer (&dvc_notification->subscriptions, g_hash_table_unref);

  g_free (dvc_notification);
}

static gboolean
notify_channels (gpointer user_data)
{
  GrdRdpDvcHandler *dvc_handler = user_data;
  GHashTableIter iter;
  DVCNotification *dvc_notification;

  g_mutex_lock (&dvc_handler->dvc_notification_mutex);
  g_hash_table_iter_init (&iter, dvc_handler->dvc_table);
  while (g_hash_table_iter_next (&iter, NULL, (gpointer *) &dvc_notification))
    {
      GHashTableIter iter2;
      DVCSubscription *dvc_subscription;

      if (dvc_notification->pending_status)
        continue;

      g_hash_table_iter_init (&iter2, dvc_notification->subscriptions);
      while (g_hash_table_iter_next (&iter2, NULL, (gpointer *) &dvc_subscription))
        {
          if (dvc_subscription->notified)
            continue;

          dvc_subscription->callback (dvc_subscription->user_data,
                                      dvc_notification->creation_status);

          dvc_subscription->notified = TRUE;
        }
    }
  g_mutex_unlock (&dvc_handler->dvc_notification_mutex);

  return G_SOURCE_CONTINUE;
}

static gboolean
source_dispatch (GSource     *source,
                 GSourceFunc  callback,
                 gpointer     user_data)
{
  g_source_set_ready_time (source, -1);

  return callback (user_data);
}

static GSourceFuncs source_funcs =
{
  .dispatch = source_dispatch,
};

static void
grd_rdp_dvc_handler_init (GrdRdpDvcHandler *dvc_handler)
{
  dvc_handler->dvc_table =
    g_hash_table_new_full (NULL, NULL,
                           NULL, dvc_notification_free);

  g_mutex_init (&dvc_handler->dvc_notification_mutex);

  dvc_handler->dvc_notification_source = g_source_new (&source_funcs,
                                                           sizeof (GSource));
  g_source_set_callback (dvc_handler->dvc_notification_source,
                         notify_channels, dvc_handler, NULL);
  g_source_set_ready_time (dvc_handler->dvc_notification_source, -1);
  g_source_attach (dvc_handler->dvc_notification_source, NULL);
}

static void
grd_rdp_dvc_handler_class_init (GrdRdpDvcHandlerClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->dispose = grd_rdp_dvc_handler_dispose;
  object_class->finalize = grd_rdp_dvc_handler_finalize;
}
