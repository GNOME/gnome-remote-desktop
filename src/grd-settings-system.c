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
 *
 */

#include "config.h"

#include "grd-settings-system.h"

#include <gio/gio.h>

#define GRD_SETTINGS_SYSTEM_GROUP_RDP "RDP"

typedef struct
{
  const char *file_key;
  const char *settings_name;
  void (* read_settings_value) (GrdSettingsSystem *settings_system,
                                GKeyFile          *key_file,
                                const char        *group,
                                const char        *key,
                                const char        *settings_name);
  void (* write_settings_value) (GrdSettingsSystem *settings_system,
                                 GKeyFile          *key_file,
                                 const char        *group,
                                 const char        *key,
                                 const char        *settings_name);
} FileSetting;

struct _GrdSettingsSystem
{
  GrdSettings parent;

  GFileMonitor *file_monitor;
};

G_DEFINE_TYPE (GrdSettingsSystem, grd_settings_system, GRD_TYPE_SETTINGS)

GrdSettingsSystem *
grd_settings_system_new (void)
{
  return g_object_new (GRD_TYPE_SETTINGS_SYSTEM,
                       "runtime-mode", GRD_RUNTIME_MODE_SYSTEM,
                       "rdp-enabled", TRUE,
                       "rdp-view-only", FALSE,
                       "rdp-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                       NULL);
}

static void
write_string (GrdSettingsSystem *settings_system,
              GKeyFile          *key_file,
              const char        *group,
              const char        *key,
              const char        *settings_name)
{
  g_autofree char *value = NULL;

  g_object_get (G_OBJECT (settings_system), settings_name, &value, NULL);

  g_key_file_set_string (key_file,
                         group,
                         key,
                         value);
}

static void
read_filename (GrdSettingsSystem *settings_system,
               GKeyFile          *key_file,
               const char        *group,
               const char        *key,
               const char        *settings_name)
{
  g_autofree char *value = NULL;
  g_autoptr (GError) error = NULL;

  value = g_key_file_get_string (key_file,
                                 group,
                                 key,
                                 &error);
  if (error && error->code != G_KEY_FILE_ERROR_KEY_NOT_FOUND)
    return;

  if (!value)
    return;

  if (!g_file_test (value, G_FILE_TEST_IS_REGULAR))
    return;

  g_object_set (G_OBJECT (settings_system), settings_name, value, NULL);
}

static void
read_int (GrdSettingsSystem *settings_system,
          GKeyFile          *key_file,
          const char        *group,
          const char        *key,
          const char        *settings_name)
{
  int value;
  g_autoptr (GError) error = NULL;

  value = g_key_file_get_integer (key_file,
                                  group,
                                  key,
                                  &error);
  if (error)
    return;

  g_object_set (G_OBJECT (settings_system), settings_name, value, NULL);
}

static void
write_int (GrdSettingsSystem *settings_system,
           GKeyFile          *key_file,
           const char        *group,
           const char        *key,
           const char        *settings_name)
{
  int value = 0;

  g_object_get (G_OBJECT (settings_system), settings_name, &value, NULL);

  g_key_file_set_integer (key_file,
                          group,
                          key,
                          value);
}

static const FileSetting rdp_file_settings[] =
{
  { "tls-cert", "rdp-server-cert-path", read_filename, write_string },
  { "tls-key", "rdp-server-key-path", read_filename, write_string },
  { "port", "rdp-port", read_int, write_int },
};

static GKeyFile *
load_key_file (const char  *file_path,
               GError     **error)
{
  g_autoptr (GKeyFile) key_file = NULL;

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_file (key_file,
                                  file_path,
                                  G_KEY_FILE_KEEP_COMMENTS |
                                  G_KEY_FILE_KEEP_TRANSLATIONS,
                                  error))
    return NULL;

  return g_steal_pointer (&key_file);
}

static void
on_rdp_setting_changed (GrdSettingsSystem *settings_system,
                        GParamSpec        *pspec,
                        FileSetting       *file_setting)
{
  g_autoptr (GKeyFile) key_file = NULL;
  g_autoptr (GError) error = NULL;

  key_file = load_key_file (GRD_CUSTOM_CONF, NULL);
  if (!key_file)
    key_file = g_key_file_new ();

  file_setting->write_settings_value (settings_system,
                                      key_file,
                                      GRD_SETTINGS_SYSTEM_GROUP_RDP,
                                      file_setting->file_key,
                                      file_setting->settings_name);

  if (!g_key_file_save_to_file (key_file, GRD_CUSTOM_CONF, &error))
    g_warning ("Failed to write %s: %s", file_setting->file_key, error->message);
}

static void
read_rdp_file_settings (GrdSettingsSystem *settings_system)
{
  g_autoptr (GKeyFile) key_file = NULL;
  g_autoptr (GError) error = NULL;
  int i;

  key_file = load_key_file (GRD_CUSTOM_CONF, NULL);
  if (!key_file)
    key_file = load_key_file (GRD_DEFAULT_CONF, &error);
  if (!key_file)
    {
      g_warning ("Failed to load default key file: %s", error->message);
      return;
    }

  for (i = 0; i < G_N_ELEMENTS (rdp_file_settings); i++)
    {
      g_signal_handlers_block_by_func (G_OBJECT (settings_system),
                                       G_CALLBACK (on_rdp_setting_changed),
                                       (gpointer) &rdp_file_settings[i]);

      rdp_file_settings[i].read_settings_value (
        settings_system,
        key_file,
        GRD_SETTINGS_SYSTEM_GROUP_RDP,
        rdp_file_settings[i].file_key,
        rdp_file_settings[i].settings_name);

      g_signal_handlers_unblock_by_func (G_OBJECT (settings_system),
                                         G_CALLBACK (on_rdp_setting_changed),
                                         (gpointer) &rdp_file_settings[i]);
    }
}

static void
on_file_changed (GFileMonitor      *file_monitor,
                 GFile             *file,
                 GFile             *other_file,
                 GFileMonitorEvent  event_type,
                 gpointer           user_data)
{
  GrdSettingsSystem *settings_system = GRD_SETTINGS_SYSTEM (user_data);

  if (event_type == G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
    read_rdp_file_settings (settings_system);
}

static void
register_read_rdp_file_settings (GrdSettingsSystem *settings_system)
{
  if (!settings_system->file_monitor)
    return;

  g_signal_connect (settings_system->file_monitor,
                    "changed", G_CALLBACK (on_file_changed),
                    settings_system);
}

static void
register_write_rdp_file_settings (GrdSettingsSystem *settings_system)
{
  int i;

  for (i = 0; i < G_N_ELEMENTS (rdp_file_settings); i++)
    {
      g_autofree char *signal_name = NULL;

      signal_name = g_strdup_printf ("notify::%s",
                                     rdp_file_settings[i].settings_name);
      g_signal_connect (G_OBJECT (settings_system),
                        signal_name,
                        G_CALLBACK (on_rdp_setting_changed),
                        (gpointer) &rdp_file_settings[i]);
    }
}

static void
grd_settings_system_constructed (GObject *object)
{
  GrdSettingsSystem *settings_system = GRD_SETTINGS_SYSTEM (object);

  read_rdp_file_settings (settings_system);

  register_read_rdp_file_settings (settings_system);
  register_write_rdp_file_settings (settings_system);

  G_OBJECT_CLASS (grd_settings_system_parent_class)->constructed (object);
}

static void
grd_settings_system_finalize (GObject *object)
{
  GrdSettingsSystem *settings = GRD_SETTINGS_SYSTEM (object);

  g_clear_object (&settings->file_monitor);

  G_OBJECT_CLASS (grd_settings_system_parent_class)->finalize (object);
}

static GFileMonitor *
init_file_monitor (void)
{
  g_autoptr (GFile) g_file = NULL;
  g_autoptr (GFileMonitor) file_monitor = NULL;
  g_autoptr (GError) error = NULL;

  g_file = g_file_new_for_path (GRD_CUSTOM_CONF);
  if (!g_file)
    return NULL;

  file_monitor = g_file_monitor_file (g_file,
                                      G_FILE_MONITOR_NONE,
                                      NULL,
                                      &error);
  if (!file_monitor)
    {
      g_warning ("Failed to monitor %s: %s", GRD_CUSTOM_CONF, error->message);
      return NULL;
    }

  return g_steal_pointer (&file_monitor);
}

static void
grd_settings_system_init (GrdSettingsSystem *settings_system)
{
  settings_system->file_monitor = init_file_monitor ();
}

static void
grd_settings_system_class_init (GrdSettingsSystemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_settings_system_constructed;
  object_class->finalize = grd_settings_system_finalize;
}
