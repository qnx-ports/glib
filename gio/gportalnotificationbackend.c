/*
* Copyright © 2016 Red Hat, Inc.
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
* Author: Matthias Clasen
*/

#include "config.h"
#include "gnotificationbackend.h"

#include <fcntl.h>

#include <glib.h>
#include <glib/gstdio.h>

#include "giomodule-priv.h"
#include "gioenumtypes.h"
#include "gicon.h"
#include "gdbusconnection.h"
#include "gapplication.h"
#include "gnotification-private.h"
#include "gportalsupport.h"
#include "gunixfdlist.h"

#define G_TYPE_PORTAL_NOTIFICATION_BACKEND  (g_portal_notification_backend_get_type ())
#define G_PORTAL_NOTIFICATION_BACKEND(o)    (G_TYPE_CHECK_INSTANCE_CAST ((o), G_TYPE_PORTAL_NOTIFICATION_BACKEND, GPortalNotificationBackend))

typedef struct _GPortalNotificationBackend GPortalNotificationBackend;
typedef GNotificationBackendClass       GPortalNotificationBackendClass;

struct _GPortalNotificationBackend
{
  GNotificationBackend parent;
};

GType g_portal_notification_backend_get_type (void);

G_DEFINE_TYPE_WITH_CODE (GPortalNotificationBackend, g_portal_notification_backend, G_TYPE_NOTIFICATION_BACKEND,
  _g_io_modules_ensure_extension_points_registered ();
  g_io_extension_point_implement (G_NOTIFICATION_BACKEND_EXTENSION_POINT_NAME,
                                 g_define_type_id, "portal", 110))

static GVariant *
adjust_serialized_media (GVariant    *serialized_media,
                         GUnixFDList *fd_list)
{
  g_autoptr(GVariant) value = NULL;
  g_autoptr(GFile) file = NULL;
  const char *key = NULL;
  g_autofree char *path = NULL;
  g_autofd int fd = -1;
  int fd_in = 0;

  if (g_variant_is_of_type (serialized_media, G_VARIANT_TYPE("(sv)")))
    g_variant_get (serialized_media, "(&sv)", &key, &value);

  if (key && value && strcmp (key, "file") == 0 && g_variant_is_of_type (value, G_VARIANT_TYPE_STRING))
    file = g_file_new_for_commandline_arg (g_variant_get_string (value, NULL));

  /* Pass through everything that isn't a native GFile */
  if (!G_IS_FILE (file) || !g_file_is_native (file))
    return serialized_media;

  path = g_file_get_path (file);
  fd = g_open (path, O_RDONLY | O_CLOEXEC);

  fd_in = g_unix_fd_list_append (fd_list, fd, NULL);

  return g_variant_new ("(sv)", "file-descriptor", g_variant_new_handle (fd_in));
}

static GVariant *
serialize_buttons (GNotification *notification)
{
  GVariantBuilder builder;
  guint n_buttons;
  guint i;
  const char *supported_purposes[] = {
    G_NOTIFICATION_BUTTON_PURPOSE_SYSTEM_CUSTOM_ALERT,
    G_NOTIFICATION_BUTTON_PURPOSE_IM_REPLY_WITH_TEXT,
    G_NOTIFICATION_BUTTON_PURPOSE_CALL_ACCEPT,
    G_NOTIFICATION_BUTTON_PURPOSE_CALL_DECLINE,
    G_NOTIFICATION_BUTTON_PURPOSE_CALL_HANG_UP,
    G_NOTIFICATION_BUTTON_PURPOSE_CALL_ENABLE_SPEAKERPHONE,
    G_NOTIFICATION_BUTTON_PURPOSE_CALL_DISABLE_SPEAKERPHONE,
    NULL
  };

  n_buttons = g_notification_get_n_buttons (notification);

  if (n_buttons == 0)
    return NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("aa{sv}"));

  for (i = 0; i < n_buttons; i++)
    {
      gchar *label;
      gchar *purpose;
      gchar *action_name;
      g_autoptr(GVariant) target = NULL;

      g_notification_get_button (notification, i, &label, &purpose, &action_name, &target);

      g_variant_builder_open (&builder, G_VARIANT_TYPE ("a{sv}"));

      g_variant_builder_add (&builder, "{sv}", "label", g_variant_new_take_string (label));
      g_variant_builder_add (&builder, "{sv}", "action", g_variant_new_take_string (action_name));

      if (purpose && (g_strv_contains (supported_purposes, purpose) || g_str_has_prefix (purpose, "x-")))
        g_variant_builder_add (&builder, "{sv}", "purpose", g_variant_new_take_string (purpose));

      if (target)
        g_variant_builder_add (&builder, "{sv}", "target", target);

      g_variant_builder_close (&builder);
    }

  return g_variant_builder_end (&builder);
}

static GVariant *
serialize_priority (GNotification *notification)
{
  g_autoptr(GEnumClass) enum_class;
  GEnumValue *value;

  enum_class = g_type_class_ref (G_TYPE_NOTIFICATION_PRIORITY);
  value = g_enum_get_value (enum_class, g_notification_get_priority (notification));
  g_assert (value != NULL);
  return g_variant_new_string (value->value_nick);
}

static GVariant *
serialize_display_hint (GNotification *notification)
{
  g_autoptr(GFlagsClass) flags_class = NULL;
  GFlagsValue *flags_value;
  GVariantBuilder builder;
  GNotificationDisplayHintFlags display_hint;
  gboolean should_show_as_new = TRUE;

  display_hint = g_notification_get_display_hint_flags (notification);

  /* If the only flag is to update the notification we don't need to set any display hints */
  if (display_hint == G_NOTIFICATION_DISPLAY_HINT_UPDATE)
    return NULL;

  flags_class = g_type_class_ref (G_TYPE_NOTIFICATION_DISPLAY_HINT_FLAGS);

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("as"));

  while (display_hint != G_NOTIFICATION_DISPLAY_HINT_NONE &&
         (flags_value = g_flags_get_first_value (flags_class, display_hint)) != NULL)
     {
      /* The display-hint 'update' needs to be serialized as 'show-as-new'
       * because we have the opposite default then the portal */
      if (flags_value->value == G_NOTIFICATION_DISPLAY_HINT_UPDATE)
        should_show_as_new = FALSE;
       else
        g_variant_builder_add (&builder, "s", flags_value->value_nick);
      display_hint &= ~flags_value->value;
    }

  if (should_show_as_new)
    g_variant_builder_add (&builder, "s", "show-as-new");

  return g_variant_builder_end (&builder);
}

static GVariant *
serialize_category (GNotification *notification)
{
  const char *category;
  const char *supported_categories[] = {
    G_NOTIFICATION_CATEGORY_IM_MESSAGE,
    G_NOTIFICATION_CATEGORY_ALARM_RINGING,
    G_NOTIFICATION_CATEGORY_CALL_INCOMING,
    G_NOTIFICATION_CATEGORY_CALL_OUTGOING,
    G_NOTIFICATION_CATEGORY_CALL_MISSED,
    G_NOTIFICATION_CATEGORY_WEATHER_WARNING_EXTREME,
    G_NOTIFICATION_CATEGORY_CELLBROADCAST_DANGER_SEVERE,
    G_NOTIFICATION_CATEGORY_CELLBROADCAST_AMBER_ALERT,
    G_NOTIFICATION_CATEGORY_CELLBROADCAST_TEST,
    G_NOTIFICATION_CATEGORY_OS_BATTERY_LOW,
    G_NOTIFICATION_CATEGORY_BROWSER_WEB_NOTIFICATION,
    NULL
  };

  category = g_notification_get_category (notification);

  /* The portal fails if we give categories that aren't supported that
   * don't start with the x-vendor prefix prefix */
  if (category && (g_strv_contains (supported_categories, category) || g_str_has_prefix (category, "x-")))
    return g_variant_new_string (category);

  return NULL;
}

static GVariant *
serialize_notification (GNotification *notification,
                        const gchar   *desktop_file_id,
                        GUnixFDList   *fd_list)
{
  GVariantBuilder builder;
  const gchar *body;
  const gchar *markup_body;
  GIcon *icon;
  GVariant *sound = NULL;
  GVariant *display_hint = NULL;
  GVariant *category;
  g_autofree gchar *default_action = NULL;
  g_autoptr(GVariant) default_action_target = NULL;
  GVariant *buttons = NULL;

  g_variant_builder_init (&builder, G_VARIANT_TYPE ("a{sv}"));

  g_variant_builder_add (&builder, "{sv}", "title", g_variant_new_string (g_notification_get_title (notification)));

  if ((body = g_notification_get_body (notification)))
    g_variant_builder_add (&builder, "{sv}", "body", g_variant_new_string (body));

  if ((markup_body = g_notification_get_markup_body (notification)))
    g_variant_builder_add (&builder, "{sv}", "markup-body", g_variant_new_string (markup_body));

  if ((icon = g_notification_get_icon (notification)))
    {
      g_autoptr(GVariant) serialized_icon = NULL;

      if ((serialized_icon = g_icon_serialize (icon)))
        g_variant_builder_add (&builder, "{sv}", "icon", adjust_serialized_media (serialized_icon, fd_list));;
    }

  if ((sound = g_notification_get_sound (notification)))
    g_variant_builder_add (&builder, "{sv}", "sound", adjust_serialized_media (sound, fd_list));
  else
    g_variant_builder_add (&builder, "{sv}", "sound", g_variant_new_string ("default"));

  g_variant_builder_add (&builder, "{sv}", "priority", serialize_priority (notification));

  if ((display_hint = serialize_display_hint (notification)))
    g_variant_builder_add (&builder, "{sv}", "display-hint", display_hint);

  if ((category = serialize_category (notification)))
    g_variant_builder_add (&builder, "{sv}", "category", category);

  if (g_notification_get_default_action (notification, &default_action, &default_action_target))
    {
      g_variant_builder_add (&builder, "{sv}", "default-action",
                                               g_variant_new_take_string (g_steal_pointer (&default_action)));

      if (default_action_target)
        g_variant_builder_add (&builder, "{sv}", "default-action-target",
                                                  default_action_target);
    }

  if ((buttons = serialize_buttons (notification)))
    g_variant_builder_add (&builder, "{sv}", "buttons", buttons);

  g_variant_builder_add (&builder, "{sv}", "desktop-file-id", g_variant_new_string (desktop_file_id));

  return g_variant_builder_end (&builder);
}

static gboolean
g_portal_notification_backend_is_supported (void)
{
  return glib_should_use_portal ();
}

static void
g_portal_notification_backend_send_notification (GNotificationBackend *backend,
                                                 const gchar          *id,
                                                 GNotification        *notification)
{
  g_autoptr(GUnixFDList) fd_list = NULL;
  g_autofree gchar *desktop_file_id = NULL;

  fd_list = g_unix_fd_list_new ();
  desktop_file_id = g_strconcat (g_application_get_application_id (backend->application), ".desktop", NULL);

  g_dbus_connection_call_with_unix_fd_list (backend->dbus_connection,
                                            "org.freedesktop.portal.Desktop",
                                            "/org/freedesktop/portal/desktop",
                                            "org.freedesktop.portal.Notification",
                                            "AddNotification",
                                            g_variant_new ("(s@a{sv})",
                                                           id,
                                                           serialize_notification (notification, desktop_file_id, fd_list)),
                                            G_VARIANT_TYPE_UNIT,
                                            G_DBUS_CALL_FLAGS_NONE, -1,
                                            fd_list,
                                            NULL, NULL, NULL);
}

static void
g_portal_notification_backend_withdraw_notification (GNotificationBackend *backend,
                                                     const gchar          *id)
{
  g_dbus_connection_call (backend->dbus_connection,
                          "org.freedesktop.portal.Desktop",
                          "/org/freedesktop/portal/desktop",
                          "org.freedesktop.portal.Notification",
                          "RemoveNotification",
                          g_variant_new ("(s)", id),
                          G_VARIANT_TYPE_UNIT,
                          G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
}

static void
g_portal_notification_backend_init (GPortalNotificationBackend *backend)
{
}

static void
g_portal_notification_backend_class_init (GPortalNotificationBackendClass *class)
{
  GNotificationBackendClass *backend_class = G_NOTIFICATION_BACKEND_CLASS (class);

  backend_class->is_supported = g_portal_notification_backend_is_supported;
  backend_class->send_notification = g_portal_notification_backend_send_notification;
  backend_class->withdraw_notification = g_portal_notification_backend_withdraw_notification;
}
