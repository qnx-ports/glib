/*
 * Copyright © 2013 Lars Uebernickel
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
 */

#include "gnotification-server.h"

#include <gio/gio.h>
#include <glib/gstdio.h>

typedef GObjectClass GNotificationServerClass;

struct _GNotificationServer
{
  GObject parent;

  GDBusConnection *connection;
  guint name_owner_id;
  guint object_id;

  gchar *backend_name;

  guint is_running;

  /* app_ids -> hashtables of notification ids -> a{sv} */
  GHashTable *applications;
};

G_DEFINE_TYPE (GNotificationServer, g_notification_server, G_TYPE_OBJECT)

enum
{
  PROP_0,
  PROP_IS_RUNNING,
  PROP_BACKEND_NAME
};

static const gchar *
get_bus_name (GNotificationServer *server) {
  if (g_strcmp0 (server->backend_name, "portal") == 0)
    return "org.freedesktop.portal.Desktop";

  return "org.gtk.Notifications";
}

static const gchar *
get_object_path (GNotificationServer *server) {
  if (g_strcmp0 (server->backend_name, "portal") == 0)
    return "/org/freedesktop/portal/desktop";

  return "/org/gtk/Notifications";
}

static GDBusInterfaceInfo *
org_gtk_Notifications_get_interface (void)
{
  static GDBusInterfaceInfo *iface_info;

  if (iface_info == NULL)
    {
      GDBusNodeInfo *info;
      GError *error = NULL;

      info = g_dbus_node_info_new_for_xml (
        "<node>"
        "  <interface name='org.gtk.Notifications'>"
        "    <method name='AddNotification'>"
        "      <arg type='s' direction='in' />"
        "      <arg type='s' direction='in' />"
        "      <arg type='a{sv}' direction='in' />"
        "    </method>"
        "    <method name='RemoveNotification'>"
        "      <arg type='s' direction='in' />"
        "      <arg type='s' direction='in' />"
        "    </method>"
        "  </interface>"
        "</node>", &error);

      if (info == NULL)
        g_error ("%s", error->message);

      iface_info = g_dbus_node_info_lookup_interface (info, "org.gtk.Notifications");
      g_assert (iface_info);

      g_dbus_interface_info_ref (iface_info);
      g_dbus_node_info_unref (info);
    }

  return iface_info;
}

static GDBusInterfaceInfo *
org_freedesktop_portal_notification_get_interface (void)
{
  static GDBusInterfaceInfo *iface_info;

  if (iface_info == NULL)
    {
      GDBusNodeInfo *info;
      GError *error = NULL;

      info = g_dbus_node_info_new_for_xml (
        "<node>"
        "  <interface name='org.freedesktop.portal.Notification'>"
        "    <method name='AddNotification'>"
        "      <arg type='s' direction='in' />"
        "      <arg type='a{sv}' direction='in' />"
        "    </method>"
        "    <method name='RemoveNotification'>"
        "      <arg type='s' direction='in' />"
        "    </method>"
        "  </interface>"
        "</node>", &error);

      if (info == NULL)
        g_error ("%s", error->message);

      iface_info = g_dbus_node_info_lookup_interface (info, "org.freedesktop.portal.Notification");
      g_assert (iface_info);

      g_dbus_interface_info_ref (iface_info);
      g_dbus_node_info_unref (info);
    }

  return iface_info;
}

static GDBusInterfaceInfo *
get_interface (GNotificationServer *server)
{
  if (g_strcmp0 (server->backend_name, "portal") == 0)
    return org_freedesktop_portal_notification_get_interface ();

  return org_gtk_Notifications_get_interface();
}

static void
g_notification_server_notification_added (GNotificationServer *server,
                                          const gchar         *app_id,
                                          const gchar         *notification_id,
                                          GVariant            *notification)
{
  GHashTable *notifications;

  notifications = g_hash_table_lookup (server->applications, app_id);
  if (notifications == NULL)
    {
      notifications = g_hash_table_new_full (g_str_hash, g_str_equal,
                                             g_free, (GDestroyNotify) g_variant_unref);
      g_hash_table_insert (server->applications, g_strdup (app_id), notifications);
    }

  g_hash_table_replace (notifications, g_strdup (notification_id), g_variant_ref (notification));

  g_signal_emit_by_name (server, "notification-received", app_id, notification_id, notification);
}

static void
g_notification_server_notification_removed (GNotificationServer *server,
                                            const gchar         *app_id,
                                            const gchar         *notification_id)
{
  GHashTable *notifications;

  notifications = g_hash_table_lookup (server->applications, app_id);
  if (notifications)
    {
      g_hash_table_remove (notifications, notification_id);
      if (g_hash_table_size (notifications) == 0)
        g_hash_table_remove (server->applications, app_id);
    }

  g_signal_emit_by_name (server, "notification-removed", app_id, notification_id);
}

static void
org_gtk_Notifications_method_call (GDBusConnection       *connection,
                                   const gchar           *sender,
                                   const gchar           *object_path,
                                   const gchar           *interface_name,
                                   const gchar           *method_name,
                                   GVariant              *parameters,
                                   GDBusMethodInvocation *invocation,
                                   gpointer               user_data)
{
  GNotificationServer *server = user_data;

  if (g_str_equal (method_name, "AddNotification"))
    {
      const gchar *app_id;
      const gchar *notification_id;
      GVariant *notification;

      g_variant_get (parameters, "(&s&s@a{sv})", &app_id, &notification_id, &notification);
      g_notification_server_notification_added (server, app_id, notification_id, notification);
      g_dbus_method_invocation_return_value (invocation, NULL);

      g_variant_unref (notification);
    }
  else if (g_str_equal (method_name, "RemoveNotification"))
    {
      const gchar *app_id;
      const gchar *notification_id;

      g_variant_get (parameters, "(&s&s)", &app_id, &notification_id);
      g_notification_server_notification_removed (server, app_id, notification_id);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else
    {
      g_dbus_method_invocation_return_dbus_error (invocation, "UnknownMethod", "No such method");
    }
}

static GVariant *
convert_serialized_fd_to_serialized_file (GVariant  *media,
                                          GUnixFDList *fd_list)
{
  gchar *key;
  g_autoptr(GVariant) handle = NULL;
  g_autofree gchar *proc_path = NULL;
  gchar path_buffer[PATH_MAX + 1];
  ssize_t symlink_size;
  gint fd_id;
  g_autofd int fd = -1;
  gchar *uri;
  GVariant *result;

  if (!g_variant_is_of_type (media, G_VARIANT_TYPE("(sv)")))
    return g_variant_ref (media);

  g_variant_get (media, "(&sv)", &key, &handle);

  if (strcmp (key, "file-descriptor") != 0)
    return g_variant_ref (media);

  g_assert_nonnull (fd_list);

  fd_id = g_variant_get_handle (handle);
  fd = g_unix_fd_list_get (fd_list, fd_id, NULL);
  g_assert_cmpint (fd, >, -1);

  proc_path = g_strdup_printf ("/proc/self/fd/%d", fd);
  symlink_size = readlink (proc_path, path_buffer, PATH_MAX);
  path_buffer[symlink_size] = 0;

  uri = g_filename_to_uri (path_buffer, NULL, NULL);
  g_assert_nonnull (uri);

  result = g_variant_new ("(sv)",
                          "file",
                          g_variant_new_take_string (uri));

  return g_variant_ref_sink (result);
}

static void
org_freedesktop_portal_notification_method_call (GDBusConnection       *connection,
                                                 const gchar           *sender,
                                                 const gchar           *object_path,
                                                 const gchar           *interface_name,
                                                 const gchar           *method_name,
                                                 GVariant              *parameters,
                                                 GDBusMethodInvocation *invocation,
                                                 gpointer               user_data)
{
  GNotificationServer *server = user_data;

  if (g_str_equal (method_name, "AddNotification"))
    {
      const gchar *notification_id;
      g_autoptr(GVariant) in_notification = NULL;
      g_autoptr(GVariant) notification = NULL;
      GUnixFDList* fd_list;
      GVariantIter iter;
      const gchar *key;
      GVariant *value;
      GVariantBuilder builder;

      fd_list = g_dbus_message_get_unix_fd_list (g_dbus_method_invocation_get_message (invocation));

      g_variant_get (parameters, "(&s@a{sv})", &notification_id, &in_notification);

      g_variant_iter_init (&iter, in_notification);
      g_variant_builder_init (&builder, G_VARIANT_TYPE_VARDICT);

      while (g_variant_iter_loop (&iter, "{sv}", &key, &value))
        {
          g_autoptr(GVariant) to_add = NULL;

          if (strcmp (key, "icon") == 0 || strcmp (key, "sound") == 0)
            to_add = convert_serialized_fd_to_serialized_file (value, fd_list);
          else
            to_add = g_variant_ref (value);

          g_variant_builder_add (&builder, "{sv}", key, to_add);
      }

      notification = g_variant_ref_sink (g_variant_builder_end (&builder));

      g_notification_server_notification_added (server, "", notification_id, notification);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else if (g_str_equal (method_name, "RemoveNotification"))
    {
      const gchar *notification_id;

      g_variant_get (parameters, "(&s)", &notification_id);
      g_notification_server_notification_removed (server, "", notification_id);
      g_dbus_method_invocation_return_value (invocation, NULL);
    }
  else
    {
      g_dbus_method_invocation_return_dbus_error (invocation, "UnknownMethod", "No such method");
    }
}

static GDBusInterfaceVTable
get_vtable (GNotificationServer *server)
{
  const GDBusInterfaceVTable vtable = {
    (g_strcmp0 (server->backend_name, "portal") == 0) ? org_freedesktop_portal_notification_method_call : org_gtk_Notifications_method_call,
    NULL, NULL, { 0 }
  };

  return vtable;
}

static void
g_notification_server_bus_acquired (GDBusConnection *connection,
                                    const gchar     *name,
                                    gpointer         user_data)
{
  GNotificationServer *server = user_data;
  const GDBusInterfaceVTable vtable = get_vtable (server);

  server->object_id = g_dbus_connection_register_object (connection, get_object_path (server),
                                                         get_interface (server),
                                                         &vtable, server, NULL, NULL);

  /* register_object only fails if the same object is exported more than once */
  g_assert (server->object_id > 0);

  server->connection = g_object_ref (connection);
}

static void
g_notification_server_name_acquired (GDBusConnection *connection,
                                     const gchar     *name,
                                     gpointer         user_data)
{
  GNotificationServer *server = user_data;

  server->is_running = TRUE;
  g_object_notify (G_OBJECT (server), "is-running");
}

static void
g_notification_server_name_lost (GDBusConnection *connection,
                                 const gchar     *name,
                                 gpointer         user_data)
{
  GNotificationServer *server = user_data;

  g_notification_server_stop (server);

  if (connection == NULL && server->connection)
    g_clear_object (&server->connection);
}

static void
g_notification_server_constructed (GObject *object)
{

  GNotificationServer *server = G_NOTIFICATION_SERVER (object);

  G_OBJECT_CLASS (g_notification_server_parent_class)->constructed (object);

  server->name_owner_id = g_bus_own_name (G_BUS_TYPE_SESSION,
                                          get_bus_name (server),
                                          G_BUS_NAME_OWNER_FLAGS_NONE,
                                          g_notification_server_bus_acquired,
                                          g_notification_server_name_acquired,
                                          g_notification_server_name_lost,
                                          server, NULL);
}

static void
g_notification_server_dispose (GObject *object)
{
  GNotificationServer *server = G_NOTIFICATION_SERVER (object);

  g_notification_server_stop (server);

  g_clear_pointer (&server->applications, g_hash_table_unref);
  g_clear_object (&server->connection);
  g_clear_pointer (&server->backend_name, g_free);

  G_OBJECT_CLASS (g_notification_server_parent_class)->dispose (object);
}

static void
g_notification_server_set_property (GObject      *object,
                                    guint         property_id,
                                    const GValue *value,
                                    GParamSpec   *pspec)
{
  GNotificationServer *server = G_NOTIFICATION_SERVER (object);

  switch (property_id)
    {
    case PROP_BACKEND_NAME:
      server->backend_name = g_value_dup_string (value);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
g_notification_server_get_property (GObject    *object,
                                    guint       property_id,
                                    GValue     *value,
                                    GParamSpec *pspec)
{
  GNotificationServer *server = G_NOTIFICATION_SERVER (object);

  switch (property_id)
    {
    case PROP_IS_RUNNING:
      g_value_set_boolean (value, server->is_running);
      break;
    case PROP_BACKEND_NAME:
      g_value_set_string (value, server->backend_name);
      break;

    default:
      G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
    }
}

static void
g_notification_server_class_init (GNotificationServerClass *class)
{
  GObjectClass *object_class = G_OBJECT_CLASS (class);

  object_class->set_property = g_notification_server_set_property;
  object_class->get_property = g_notification_server_get_property;
  object_class->constructed = g_notification_server_constructed;
  object_class->dispose = g_notification_server_dispose;

  g_object_class_install_property (object_class, PROP_IS_RUNNING,
                                   g_param_spec_boolean ("is-running", "", "", FALSE,
                                                         G_PARAM_READABLE | G_PARAM_STATIC_STRINGS));

  g_object_class_install_property (object_class, PROP_BACKEND_NAME,
                                   g_param_spec_string ("backend-name", "", "", NULL,
                                                         G_PARAM_READWRITE | G_PARAM_CONSTRUCT_ONLY | G_PARAM_STATIC_STRINGS));

  g_signal_new ("notification-received", G_TYPE_NOTIFICATION_SERVER, G_SIGNAL_RUN_FIRST,
                0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 3,
                G_TYPE_STRING, G_TYPE_STRING, G_TYPE_VARIANT);

  g_signal_new ("notification-removed", G_TYPE_NOTIFICATION_SERVER, G_SIGNAL_RUN_FIRST,
                0, NULL, NULL, g_cclosure_marshal_generic, G_TYPE_NONE, 2,
                G_TYPE_STRING, G_TYPE_STRING);
}

static void
g_notification_server_init (GNotificationServer *server)
{
  server->applications = g_hash_table_new_full (g_str_hash, g_str_equal,
                                                g_free, (GDestroyNotify) g_hash_table_unref);

}

GNotificationServer *
g_notification_server_new (const gchar *backend_name)
{
  return g_object_new (G_TYPE_NOTIFICATION_SERVER,
                       "backend-name", backend_name,
                       NULL);
}

void
g_notification_server_stop (GNotificationServer *server)
{
  g_return_if_fail (G_IS_NOTIFICATION_SERVER (server));

  if (server->name_owner_id)
    {
      g_bus_unown_name (server->name_owner_id);
      server->name_owner_id = 0;
    }

  if (server->object_id && server->connection)
    {
      g_dbus_connection_unregister_object (server->connection, server->object_id);
      server->object_id = 0;
    }

  if (server->is_running)
    {
      server->is_running = FALSE;
      g_object_notify (G_OBJECT (server), "is-running");
    }
}

gboolean
g_notification_server_get_is_running (GNotificationServer *server)
{
  g_return_val_if_fail (G_IS_NOTIFICATION_SERVER (server), FALSE);

  return server->is_running;
}

gchar **
g_notification_server_list_applications (GNotificationServer *server)
{
  g_return_val_if_fail (G_IS_NOTIFICATION_SERVER (server), NULL);

  return (gchar **) g_hash_table_get_keys_as_array (server->applications, NULL);
}

gchar **
g_notification_server_list_notifications (GNotificationServer *server,
                                          const gchar         *app_id)
{
  GHashTable *notifications;

  g_return_val_if_fail (G_IS_NOTIFICATION_SERVER (server), NULL);
  g_return_val_if_fail (app_id != NULL, NULL);

  notifications = g_hash_table_lookup (server->applications, app_id);

  if (notifications == NULL)
    return NULL;

  return (gchar **) g_hash_table_get_keys_as_array (notifications, NULL);
}
