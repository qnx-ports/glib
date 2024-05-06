/*
 * Copyright © 2013 Lars Uebernickel
 * Copyright © 2024 GNOME Foundation Inc.
 *
 * SPDX-License-Identifier: LGPL-2.1-or-later
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General
 * Public License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Authors: Lars Uebernickel <lars@uebernic.de>
 *          Julian Sparber <jsparber@gnome.org>
 */

#include <glib.h>

#include "gnotification-server.h"
#include "gdbus-sessionbus.h"

static GPtrArray *notifications = NULL;

struct _GNotification
{
  GObject parent;

  gchar *title;
  gchar *body;
  gchar *markup_body;
  GIcon *icon;
  GVariant *sound;
  GNotificationPriority priority;
  gchar *category;
  GNotificationDisplayHintFlags display_hint;
  GPtrArray *buttons;
  gchar *default_action;
  GVariant *default_action_target;
};

typedef struct
{
  gchar *label;
  gchar *purpose;
  gchar *action_name;
  GVariant *target;
} Button;

typedef struct
{
  gchar *desktop_file_id;
  gchar *id;
  GNotification *notification;
} SendData;

static void
send_data_free (gpointer pointer)
{
  SendData *data = pointer;

  g_free (data->desktop_file_id);
  g_free (data->id);
  g_object_unref (data->notification);
}

static void
store_and_send (GApplication  *application,
                const gchar   *id,
                GNotification *notification)
{

  SendData *data = g_new0 (SendData, 1);

  data->desktop_file_id = g_strconcat (g_application_get_application_id (application), ".desktop", NULL);
  data->id = g_strdup (id);
  data->notification  = notification;
  g_ptr_array_add (notifications, data);
  g_application_send_notification (application, id, notification);
}

static GFile *
get_empty_file (void) {
  GFile *file = NULL;
  g_autoptr(GFileIOStream) iostream = NULL;
  GOutputStream *stream = NULL;

  file = g_file_new_tmp ("iconXXXXXX", &iostream, NULL);
  stream = g_io_stream_get_output_stream (G_IO_STREAM (iostream));
  g_output_stream_write_all (stream, "", 0, NULL, NULL, NULL);
  g_output_stream_close (stream, NULL, NULL);

  return file;
}

static void
activate_app (GApplication *application,
              gpointer      user_data)
{
  g_autoptr(GNotification) notification = NULL;
  g_autoptr(GIcon) icon = NULL;
  g_autoptr(GBytes) bytes = NULL;
  g_autoptr(GFile) file = NULL;

  if (!notifications)
    notifications = g_ptr_array_new_with_free_func (send_data_free);

  bytes = g_bytes_new_static (NULL, 0);
  file = get_empty_file ();

  notification = g_notification_new ("Test");
  store_and_send (application, "test1", notification);

  notification = g_notification_new ("Test2");
  store_and_send (application, "test2", notification);

  g_application_withdraw_notification (application, "test1");

  notification = g_notification_new ("Test3");
  store_and_send (application, "test3", notification);

  notification = g_notification_new ("Test4");
  icon = g_themed_icon_new ("i-c-o-n");
  g_notification_set_icon (notification, icon);
  g_clear_object (&icon);
  g_notification_set_body (notification, "body");
  g_notification_set_markup_body (notification, "markup-body");
  g_notification_set_priority (notification, G_NOTIFICATION_PRIORITY_URGENT);
  g_notification_set_default_action_and_target (notification, "app.action", "i", 42);
  g_notification_add_button_with_purpose_and_target (notification, "label", "x-gnome.purpose", "app.action2", "s", "bla");
  g_notification_set_category (notification, "x-gnome.category");
  g_notification_set_display_hint_flags (notification, G_NOTIFICATION_DISPLAY_HINT_TRANSIENT);
  store_and_send (application, "test4", notification);

  notification = g_notification_new ("Test5");
  icon = g_file_icon_new (file);
  g_notification_set_icon (notification, icon);
  g_clear_object (&icon);
  store_and_send (application, "test5", notification);

  notification = g_notification_new ("Test6");
  icon = g_bytes_icon_new (bytes);
  g_notification_set_icon (notification, icon);
  g_clear_object (&icon);
  store_and_send (application, "test6", notification);

  notification = g_notification_new ("Test7");
  g_notification_set_silent (notification, TRUE);
  store_and_send (application, "test7", notification);

  notification = g_notification_new ("Test8");
  g_notification_set_sound_from_file (notification, file);
  store_and_send (application, "test8", notification);

  notification = g_notification_new ("Test9");
  g_notification_set_sound_from_bytes (notification, bytes);
  store_and_send (application, "test9", notification);

  store_and_send (application, NULL, notification);

  g_dbus_connection_flush_sync (g_application_get_dbus_connection (application), NULL, NULL);
}

static void
notification_received (GNotificationServer *server,
                       const gchar         *app_id,
                       const gchar         *notification_id,
                       GVariant            *notification,
                       gpointer             user_data)
{
  gint *count = user_data;
  SendData *exp_data;
  struct _GNotification *exp_notification;
  const gchar *desktop_file_id = NULL;

  exp_data = g_ptr_array_index (notifications, *count);
  g_assert_nonnull (exp_data);
  exp_notification = (struct _GNotification *)exp_data->notification;
  g_assert_nonnull (exp_notification);

  g_assert_true (g_variant_lookup (notification, "desktop-file-id", "&s", &desktop_file_id));
  g_assert_cmpstr (desktop_file_id, ==, exp_data->desktop_file_id);

  if (exp_data->id)
    g_assert_cmpstr (exp_data->id, ==, notification_id);
  else
    g_assert_true (g_dbus_is_guid (notification_id));

  if (exp_notification->title)
    {
      const gchar *title;
      g_assert_true (g_variant_lookup (notification, "title", "&s", &title));
      g_assert_cmpstr (title, ==, exp_notification->title);
    }

  if (exp_notification->body)
    {
      const gchar *body;
      g_assert_true (g_variant_lookup (notification, "body", "&s", &body));
      g_assert_cmpstr (body, ==, exp_notification->body);
    }

  if (exp_notification->markup_body)
    {
      const gchar *body;
      g_assert_true (g_variant_lookup (notification, "markup-body", "&s", &body));
      g_assert_cmpstr (body, ==, exp_notification->markup_body);
    }

  if (exp_notification->icon)
    {
      g_autoptr(GVariant) serialized_icon = NULL;
      g_autoptr(GIcon) icon = NULL;

      serialized_icon = g_variant_lookup_value (notification, "icon", NULL);
      icon = g_icon_deserialize (serialized_icon);
      g_assert_true (g_icon_equal (exp_notification->icon, icon));
    }

  if (exp_notification->sound)
    {
      g_autoptr(GVariant) sound = NULL;

      sound = g_variant_lookup_value (notification, "sound", NULL);
      g_assert_true (g_variant_equal (sound, exp_notification->sound));
    }

  if (exp_notification->priority)
    {
      g_autoptr(GEnumClass) enum_class = NULL;
      GEnumValue *enum_value;
      const gchar *priority = NULL;
      g_assert_true (g_variant_lookup (notification, "priority", "&s", &priority));

      enum_class = g_type_class_ref (G_TYPE_NOTIFICATION_PRIORITY);
      g_assert_nonnull (enum_class);
      enum_value = g_enum_get_value_by_nick (enum_class, priority);
      g_assert_nonnull (enum_value);

      g_assert_true ((GNotificationPriority) enum_value->value == exp_notification->priority);
    }

  if (exp_notification->display_hint)
    {
      g_autoptr(GFlagsClass) flags_class = NULL;
      GNotificationDisplayHintFlags display_hint = G_NOTIFICATION_DISPLAY_HINT_UPDATE;
      const gchar** flags = NULL;
      gsize i;
      g_assert_true (g_variant_lookup (notification, "display-hint", "^a&s", &flags));

      flags_class = g_type_class_ref (G_TYPE_NOTIFICATION_DISPLAY_HINT_FLAGS);
      g_assert_nonnull (flags_class);

      for (i = 0; flags[i]; i++)
        {
          GFlagsValue *flags_value;

          if (g_strcmp0 (flags[i], "show-as-new") == 0)
            {
              display_hint &= ~G_NOTIFICATION_DISPLAY_HINT_UPDATE;
              continue;
            }

          flags_value = g_flags_get_value_by_nick (flags_class, flags[i]);
          g_assert_nonnull (flags_value);
          display_hint |= flags_value->value;
        }

      g_assert_true (display_hint == exp_notification->display_hint);
    }

  if (exp_notification->category)
    {
      const gchar *category;
      g_assert_true (g_variant_lookup (notification, "category", "&s", &category));
      g_assert_cmpstr (category, ==, exp_notification->category);
    }

  if (exp_notification->default_action)
    {
      const gchar *default_action;
      g_assert_true (g_variant_lookup (notification, "default-action", "&s", &default_action));
      g_assert_cmpstr (default_action, ==, exp_notification->default_action);
    }

  if (exp_notification->default_action_target)
    {
      g_autoptr(GVariant) default_action_target = NULL;
      default_action_target = g_variant_lookup_value (notification, "default-action-target", NULL);
      g_assert_true (g_variant_equal (default_action_target, exp_notification->default_action_target));
    }

  if (exp_notification->buttons && exp_notification->buttons->len > 0)
    {
      gsize i;
      g_autoptr(GVariant) buttons = NULL;
      buttons = g_variant_lookup_value (notification, "buttons", G_VARIANT_TYPE("aa{sv}"));
      g_assert_nonnull (buttons);

      for (i = 0; i < g_variant_n_children (buttons); i++)
        {
          Button *exp_button;
          g_autoptr(GVariant) button = NULL;
          const gchar *label = NULL;
          const gchar *purpose = NULL;
          const gchar *action_name = NULL;
          g_autoptr(GVariant) action_target = NULL;

          button = g_variant_get_child_value (buttons, i);
          g_assert_nonnull (button);

          exp_button = (Button*)g_ptr_array_index (exp_notification->buttons, i);
          g_assert_nonnull (exp_button);

          if (exp_button->label)
            {
              g_assert_true (g_variant_lookup (button, "label", "&s", &label));
              g_assert_cmpstr (label, ==, exp_button->label);
            }

          if (exp_button->purpose)
            {
              g_assert_true (g_variant_lookup (button, "purpose", "&s", &purpose));
              g_assert_cmpstr (purpose, ==, exp_button->purpose);
            }

          if (exp_button->action_name)
            {
              g_assert_true (g_variant_lookup (button, "action", "&s", &action_name));
              g_assert_cmpstr (action_name, ==, exp_button->action_name);
            }

          action_target = g_variant_lookup_value (button, "target", NULL);
          g_assert_true (g_variant_equal (action_target, exp_button->target));
        }
    }

  if (*count == 9)
    g_notification_server_stop (server);

  (*count)++;
}

static void
notification_removed (GNotificationServer *server,
                      const gchar         *app_id,
                      const gchar         *notification_id,
                      gpointer             user_data)
{
  gint *count = user_data;

  g_assert_cmpstr (notification_id, ==, "test1");

  (*count)++;
}

static void
server_notify_is_running (GObject    *object,
                          GParamSpec *pspec,
                          gpointer    user_data)
{
  GMainLoop *loop = user_data;
  GNotificationServer *server = G_NOTIFICATION_SERVER (object);

  if (g_notification_server_get_is_running (server))
    {
      GApplication *app;

      app = g_application_new ("org.gtk.TestApplication", G_APPLICATION_DEFAULT_FLAGS);
      g_signal_connect (app, "activate", G_CALLBACK (activate_app), NULL);

      g_application_run (app, 0, NULL);

      g_object_unref (app);
    }
  else
    {
      g_main_loop_quit (loop);
    }
}

static void
basic (void)
{
  GNotificationServer *server;
  GMainLoop *loop;
  gint received_count = 0;
  gint removed_count = 0;

  session_bus_up ();

  loop = g_main_loop_new (NULL, FALSE);

  g_setenv ("GIO_USE_PORTALS", "1", TRUE);

  server = g_notification_server_new ("portal");
  g_signal_connect (server, "notification-received", G_CALLBACK (notification_received), &received_count);
  g_signal_connect (server, "notification-removed", G_CALLBACK (notification_removed), &removed_count);
  g_signal_connect (server, "notify::is-running", G_CALLBACK (server_notify_is_running), loop);

  g_main_loop_run (loop);

  g_assert_cmpint (received_count, ==, 10);
  g_assert_cmpint (removed_count, ==, 1);

  g_object_unref (server);
  g_main_loop_unref (loop);
  session_bus_stop ();
}

int main (int argc, char *argv[])
{
  g_test_init (&argc, &argv, NULL);

  g_test_add_func ("/portal-notification-backend/basic", basic);

  return g_test_run ();
}
