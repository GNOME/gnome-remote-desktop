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
#include <glib/gstdio.h>

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

typedef enum
{
  GRD_SETTINGS_SOURCE_TYPE_INVALID = -1,
  GRD_SETTINGS_SOURCE_TYPE_DEFAULT = 0,
  GRD_SETTINGS_SOURCE_TYPE_CUSTOM = 1,
  GRD_SETTINGS_SOURCE_TYPE_LOCAL_STATE = 2,
  NUMBER_OF_GRD_SETTINGS_SOURCES
} GrdSettingsSourceType;

typedef struct
{
  GKeyFile *key_file;
  GFileMonitor *file_monitor;
  GrdSettingsSourceType type;
} GrdSettingsSource;

struct _GrdSettingsSystem
{
  GrdSettings parent;

  GrdSettingsSource *setting_sources[NUMBER_OF_GRD_SETTINGS_SOURCES];

  GKeyFile *key_file;

  gboolean use_local_state;
};

static void on_file_changed (GFileMonitor      *file_monitor,
                             GFile             *file,
                             GFile             *other_file,
                             GFileMonitorEvent  event_type,
                             gpointer           user_data);
static void read_rdp_file_settings (GrdSettingsSystem *settings_system);

G_DEFINE_TYPE (GrdSettingsSystem, grd_settings_system, GRD_TYPE_SETTINGS)

static GrdSettingsSource *
grd_settings_source_new (GrdSettingsSourceType  source_type,
                         const char            *file_path)
{
  g_autoptr (GError) error = NULL;
  g_autoptr (GFile) file = NULL;
  g_autofree GrdSettingsSource *source = g_new0 (GrdSettingsSource, 1);
  g_autoptr (GKeyFile) key_file = NULL;
  GKeyFileFlags flags = G_KEY_FILE_NONE;

  key_file = g_key_file_new ();

  if (source_type == GRD_SETTINGS_SOURCE_TYPE_CUSTOM)
    flags |= G_KEY_FILE_KEEP_COMMENTS;

  if  (!g_key_file_load_from_file (key_file,
                                   file_path,
                                   flags,
                                   &error))
    {
      g_debug ("Failed to load key file from '%s': %s", file_path, error->message);
      return NULL;
    }

  source->key_file = g_steal_pointer (&key_file);

  file = g_file_new_for_path (file_path);
  source->file_monitor = g_file_monitor_file (file, G_FILE_MONITOR_NONE, NULL, &error);
  if  (!source->file_monitor)
    {
      g_warning ("Failed to monitor file '%s': %s", file_path, error->message);
      return NULL;
    }

  return g_steal_pointer (&source);
}

static void
grd_settings_source_free (GrdSettingsSource *source)
{
  if (!source)
    return;

  g_clear_pointer (&source->key_file, g_key_file_unref);
  g_clear_object (&source->file_monitor);
  g_clear_pointer (&source, g_free);
}

static char *
grd_settings_system_get_local_state_conf (void)
{
  return g_build_filename (g_get_user_data_dir (), "gnome-remote-desktop", "grd.conf", NULL);
}

static void
merge_descendant_keys (GrdSettingsSystem *settings_system,
                       GKeyFile          *key_file,
                       GKeyFile          *descendant_key_file)
{
  g_auto (GStrv) groups = NULL;
  size_t group_count = 0;
  size_t i, j;

  g_assert (key_file != NULL);
  g_assert (descendant_key_file != NULL);

  groups = g_key_file_get_groups (descendant_key_file, &group_count);
  for (i = 0; i < group_count; i++)
    {
      g_auto (GStrv) keys = NULL;
      size_t key_count = 0;

      keys = g_key_file_get_keys (descendant_key_file, groups[i], &key_count, NULL);
      for (j = 0; j < key_count; j++)
        {
          g_autofree char *value = g_key_file_get_value (descendant_key_file, groups[i], keys[j], NULL);

          if (value)
            g_key_file_set_value (key_file, groups[i], keys[j], value);
        }
    }
}

static gboolean
prune_inherited_keys (GrdSettingsSystem     *settings_system,
                      GrdSettingsSourceType  source_type,
                      GKeyFile              *key_file_to_prune)
{
  g_auto (GStrv) groups = NULL;
  size_t group_count = 0;
  size_t i, j;

  if (source_type == GRD_SETTINGS_SOURCE_TYPE_DEFAULT)
    return TRUE;

  if (source_type != GRD_SETTINGS_SOURCE_TYPE_CUSTOM)
    g_key_file_remove_comment (key_file_to_prune, NULL, NULL, NULL);

  groups = g_key_file_get_groups (key_file_to_prune, &group_count);

  for (i = 0; i < group_count; i++)
    {
      g_auto (GStrv) keys = NULL;
      size_t key_count;

      if (source_type != GRD_SETTINGS_SOURCE_TYPE_CUSTOM)
        g_key_file_remove_comment (key_file_to_prune, groups[i], NULL, NULL);

      keys = g_key_file_get_keys (key_file_to_prune, groups[i], &key_count, NULL);

      for (j = 0; j < key_count; j++)
        {
          g_autofree char *value_to_prune = g_key_file_get_value (key_file_to_prune, groups[i], keys[j], NULL);
          GrdSettingsSourceType ancestor_type;
          gboolean should_prune_key = FALSE;

          if (source_type != GRD_SETTINGS_SOURCE_TYPE_CUSTOM)
            g_key_file_remove_comment (key_file_to_prune, groups[i], keys[j], NULL);

          for (ancestor_type = source_type - 1; ancestor_type >= GRD_SETTINGS_SOURCE_TYPE_DEFAULT; ancestor_type--)
            {
              GrdSettingsSource *ancestor_source;

              ancestor_source = settings_system->setting_sources[ancestor_type];
              if (ancestor_source == NULL)
                continue;

              if (g_key_file_has_key (ancestor_source->key_file, groups[i], keys[j], NULL))
                {
                  g_autofree char *ancestor_value = g_key_file_get_value (ancestor_source->key_file, groups[i], keys[j], NULL);

                  if (g_strcmp0 (value_to_prune, ancestor_value) == 0)
                    should_prune_key = TRUE;

                  break;
                }
            }

          if (should_prune_key)
            g_key_file_remove_key (key_file_to_prune, groups[i], keys[j], NULL);
        }
      g_clear_pointer (&keys, g_strfreev);

      keys = g_key_file_get_keys (key_file_to_prune, groups[i], &key_count, NULL);

      if (key_count == 0)
        g_key_file_remove_group (key_file_to_prune, groups[i], NULL);
    }
  g_clear_pointer (&groups, g_strfreev);

  groups = g_key_file_get_groups (key_file_to_prune, &group_count);

  if (group_count == 0)
    return FALSE;

  return TRUE;
}

static char **
get_conf_paths (void)
{
  g_autofree char *local_state_conf = NULL;
  char **paths = NULL;

  local_state_conf = grd_settings_system_get_local_state_conf ();

  paths = g_new (char *, NUMBER_OF_GRD_SETTINGS_SOURCES + 1);
  paths[GRD_SETTINGS_SOURCE_TYPE_DEFAULT] = g_strdup (GRD_DEFAULT_CONF);
  paths[GRD_SETTINGS_SOURCE_TYPE_CUSTOM] = g_strdup (GRD_CUSTOM_CONF);
  paths[GRD_SETTINGS_SOURCE_TYPE_LOCAL_STATE] = g_steal_pointer (&local_state_conf);
  paths[NUMBER_OF_GRD_SETTINGS_SOURCES] = NULL;

  return paths;
}

static void
grd_settings_system_reload_sources (GrdSettingsSystem *settings_system)
{
  g_autoptr (GKeyFile) key_file = NULL;
  g_autoptr (GError) error = NULL;
  g_auto (GStrv) paths = NULL;
  size_t source_type;

  paths = get_conf_paths ();
  key_file = g_key_file_new ();

  for (source_type = GRD_SETTINGS_SOURCE_TYPE_DEFAULT;
       source_type < NUMBER_OF_GRD_SETTINGS_SOURCES;
       source_type++)
    {
      GrdSettingsSource *source = NULL;

      g_clear_pointer (&settings_system->setting_sources[source_type],
                       grd_settings_source_free);

      source = grd_settings_source_new (source_type, paths[source_type]);

      if (source != NULL)
        {
          merge_descendant_keys (settings_system, key_file, source->key_file);

          g_signal_connect (source->file_monitor,
                            "changed", G_CALLBACK (on_file_changed),
                            settings_system);

          settings_system->setting_sources[source_type] = g_steal_pointer (&source);
        }
    }

  g_clear_pointer (&settings_system->key_file, g_key_file_unref);
  settings_system->key_file = g_steal_pointer (&key_file);
}

static void
grd_settings_system_free_sources (GrdSettingsSystem *settings_system)
{
  size_t i;

  for (i = 0; i < NUMBER_OF_GRD_SETTINGS_SOURCES; i++)
    g_clear_pointer (&settings_system->setting_sources[i],
                     grd_settings_source_free);
}

GrdSettingsSystem *
grd_settings_system_new (void)
{
  return g_object_new (GRD_TYPE_SETTINGS_SYSTEM,
                       "runtime-mode", GRD_RUNTIME_MODE_SYSTEM,
                       "rdp-view-only", FALSE,
                       "rdp-screen-share-mode", GRD_RDP_SCREEN_SHARE_MODE_EXTEND,
                       NULL);
}

void
grd_settings_system_use_local_state (GrdSettingsSystem *settings_system)
{
  g_assert (GRD_IS_SETTINGS_SYSTEM (settings_system));

  settings_system->use_local_state = TRUE;
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

static void
read_boolean (GrdSettingsSystem *settings_system,
              GKeyFile          *key_file,
              const char        *group,
              const char        *key,
              const char        *settings_name)
{
  gboolean value = FALSE;
  g_autoptr (GError) error = NULL;

  value = g_key_file_get_boolean (key_file, group, key, &error);
  if (error)
    return;

  g_object_set (G_OBJECT (settings_system), settings_name, value, NULL);
}

static void
write_boolean (GrdSettingsSystem *settings_system,
               GKeyFile          *key_file,
               const char        *group,
               const char        *key,
               const char        *settings_name)
{
  gboolean value = FALSE;

  g_object_get (G_OBJECT (settings_system), settings_name, &value, NULL);

  g_key_file_set_boolean (key_file,
                          group,
                          key,
                          value);
}


static const FileSetting rdp_file_settings[] =
{
  { "enabled", "rdp-enabled", read_boolean, write_boolean },
  { "tls-cert", "rdp-server-cert-path", read_filename, write_string },
  { "tls-key", "rdp-server-key-path", read_filename, write_string },
  { "port", "rdp-port", read_int, write_int },
};

static void
on_rdp_setting_changed (GrdSettingsSystem *settings_system,
                        GParamSpec        *pspec,
                        FileSetting       *file_setting)
{
  g_autoptr (GKeyFile) key_file = NULL;
  g_autofree char *data = NULL;
  size_t length = 0;
  g_autoptr (GError) error = NULL;
  const char *filename;
  GrdSettingsSourceType settings_source_type;
  g_autofree char *local_state_conf = grd_settings_system_get_local_state_conf ();

  grd_settings_system_reload_sources (settings_system);

  file_setting->write_settings_value (settings_system,
                                      settings_system->key_file,
                                      GRD_SETTINGS_SYSTEM_GROUP_RDP,
                                      file_setting->file_key,
                                      file_setting->settings_name);

  data = g_key_file_to_data (settings_system->key_file, &length, NULL);

  key_file = g_key_file_new ();
  if (!g_key_file_load_from_data (key_file, data, length,
                                  G_KEY_FILE_KEEP_COMMENTS |
                                  G_KEY_FILE_KEEP_TRANSLATIONS,
                                  &error))
    {
      g_warning ("Failed to copy loaded key file: %s", error->message);
      return;
    }

  if (settings_system->use_local_state)
    settings_source_type = GRD_SETTINGS_SOURCE_TYPE_LOCAL_STATE;
  else
    settings_source_type = GRD_SETTINGS_SOURCE_TYPE_CUSTOM;

  switch (settings_source_type)
    {
      case GRD_SETTINGS_SOURCE_TYPE_LOCAL_STATE:
        filename = local_state_conf;
        break;

      case GRD_SETTINGS_SOURCE_TYPE_CUSTOM:
        g_remove (local_state_conf);
        filename = GRD_CUSTOM_CONF;
        break;

      default:
        g_assert_not_reached ();

    }

  if (prune_inherited_keys (settings_system,
                            settings_source_type,
                            key_file))
    {
      if (!g_key_file_save_to_file (key_file, filename, &error))
        g_warning ("Failed to write %s: %s", file_setting->file_key, error->message);
    }
  else
    {
      g_remove (filename);
    }
}

static void
read_rdp_file_settings (GrdSettingsSystem *settings_system)
{
  g_autoptr (GKeyFile) key_file = NULL;
  g_autoptr (GError) error = NULL;
  int i;

  for (i = 0; i < G_N_ELEMENTS (rdp_file_settings); i++)
    {
      g_signal_handlers_block_by_func (G_OBJECT (settings_system),
                                       G_CALLBACK (on_rdp_setting_changed),
                                       (gpointer) &rdp_file_settings[i]);

      rdp_file_settings[i].read_settings_value (
        settings_system,
        settings_system->key_file,
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
    {
      grd_settings_system_reload_sources (settings_system);
      read_rdp_file_settings (settings_system);
    }
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

  grd_settings_system_reload_sources (settings_system);
  read_rdp_file_settings (settings_system);

  register_write_rdp_file_settings (settings_system);

  G_OBJECT_CLASS (grd_settings_system_parent_class)->constructed (object);
}

static void
grd_settings_system_finalize (GObject *object)
{
  GrdSettingsSystem *settings = GRD_SETTINGS_SYSTEM (object);

  grd_settings_system_free_sources (settings);

  G_OBJECT_CLASS (grd_settings_system_parent_class)->finalize (object);
}

static void
grd_settings_system_init (GrdSettingsSystem *settings_system)
{
}

static void
grd_settings_system_class_init (GrdSettingsSystemClass *klass)
{
  GObjectClass *object_class = G_OBJECT_CLASS (klass);

  object_class->constructed = grd_settings_system_constructed;
  object_class->finalize = grd_settings_system_finalize;
}
