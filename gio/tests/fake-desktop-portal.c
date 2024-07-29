/*
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
 * Authors: Julian Sparber <jsparber@gnome.org>
 */

/* A stub implementation of xdg-dekstop-portal */

#include <glib.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include "fake-openuri-portal-generated.h"
#include "fake-request-portal-generated.h"

static gboolean
on_handle_close (FakeRequest           *object,
                 GDBusMethodInvocation *invocation,
                 gpointer               user_data)
{
  g_test_message ("Got request close");
  fake_request_complete_close (object, invocation);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static char*
get_request_path (GDBusMethodInvocation *invocation,
                  const char            *token) {
  char *request_obj_path;
  char *sender;
  int i;

  sender = g_strdup (g_dbus_method_invocation_get_sender (invocation) + 1);

  for (i = 0; sender[i]; i++)
    if (sender[i] == '.')
      sender[i] = '_';

  request_obj_path = g_strdup_printf ("/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
  g_free (sender);

  return request_obj_path;
}

static gboolean
on_handle_open_file (FakeOpenURI           *object,
                     GDBusMethodInvocation *invocation,
                     GUnixFDList           *fd_list,
                     const gchar           *arg_parent_window,
                     GVariant              *arg_fd,
                     GVariant              *arg_options)
{
  char *request_obj_path;
  GVariantBuilder opt_builder;
  GError *error = NULL;
  FakeRequest *interfaceRequest;
  const char *activation_token;
  const char *token;

  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);

  //TODO: make sure that activation-token and window is expected and other stuff
  // Write the data to a file so it can be read by the test

  g_variant_lookup (arg_options, "handle_token", "&s", &token);

  g_test_message ("Got open file request");

  request_obj_path = get_request_path (invocation, token);
  interfaceRequest = fake_request_skeleton_new ();
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (interfaceRequest,
                    "handle-close",
                    G_CALLBACK (on_handle_close),
                    NULL);

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interfaceRequest),
                                    g_dbus_method_invocation_get_connection (invocation),
                                    request_obj_path,
                                    &error);
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (interfaceRequest),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_assert_no_error (error);
  g_test_message ("Request skeleton exported at %s", request_obj_path);

  fake_open_uri_complete_open_file (object,
                                    invocation,
                                    NULL,
                                    request_obj_path);

  fake_request_emit_response (interfaceRequest,
                              0, /* Success */
                              g_variant_builder_end (&opt_builder));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (interfaceRequest));
  g_test_message ("Response emitted");
  g_free (request_obj_path);

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static gboolean
on_handle_open_uri (FakeOpenURI           *object,
                    GDBusMethodInvocation *invocation,
                    const gchar           *arg_parent_window,
                    GVariant              *arg_uri,
                    GVariant              *arg_options)
{
  char *request_obj_path;
  GVariantBuilder opt_builder;
  FakeRequest *interfaceRequest;
  GError *error = NULL;
  const char *activation_token;
  const char *token;

  g_variant_lookup (arg_options, "activation_token", "&s", &activation_token);

  //TODO: make sure that activation-token and window is expected and other stuff
  // Write the data to a file so it can be read by the test

  g_variant_lookup (arg_options, "handle_token", "&s", &token);

  g_test_message ("Got open uri request");

  request_obj_path = get_request_path (invocation, token);
  interfaceRequest = fake_request_skeleton_new ();
  g_variant_builder_init (&opt_builder, G_VARIANT_TYPE_VARDICT);

  g_signal_connect (interfaceRequest,
                    "handle-close",
                    G_CALLBACK (on_handle_close),
                    NULL);

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interfaceRequest),
                                    g_dbus_method_invocation_get_connection (invocation),
                                    request_obj_path,
                                    &error);

  g_assert_no_error (error);
  g_dbus_interface_skeleton_set_flags (G_DBUS_INTERFACE_SKELETON (interfaceRequest),
                                       G_DBUS_INTERFACE_SKELETON_FLAGS_HANDLE_METHOD_INVOCATIONS_IN_THREAD);
  g_test_message ("Request skeleton exported at %s", request_obj_path);

  fake_open_uri_complete_open_uri (object,
                                   invocation,
                                   request_obj_path);

  fake_request_emit_response (interfaceRequest,
                              0, /* Success */
                              g_variant_builder_end (&opt_builder));

  g_dbus_interface_skeleton_unexport (G_DBUS_INTERFACE_SKELETON (interfaceRequest));
  g_free (request_obj_path);

  g_test_message ("Response emitted");

  return G_DBUS_METHOD_INVOCATION_HANDLED;
}

static void
on_bus_acquired (GDBusConnection *connection,
                 const gchar     *name,
                 gpointer         user_data)
{
  FakeOpenURI *interfaceOpenURI;
  GError *error = NULL;

  g_test_message ("Acquired a message bus connection");

  interfaceOpenURI = fake_open_uri_skeleton_new ();

  g_signal_connect (interfaceOpenURI,
                    "handle-open-file",
                    G_CALLBACK (on_handle_open_file),
                    NULL);
  g_signal_connect (interfaceOpenURI,
                    "handle-open-uri",
                    G_CALLBACK (on_handle_open_uri),
                    NULL);

  g_dbus_interface_skeleton_export (G_DBUS_INTERFACE_SKELETON (interfaceOpenURI),
                                    connection,
                                    "/org/freedesktop/portal/desktop",
                                    &error);
  g_assert_no_error (error);
}

static void
on_name_acquired (GDBusConnection *connection,
                  const gchar     *name,
                  gpointer         user_data)
{
  g_test_message ("Acquired the name %s", name);
}

static void
on_name_lost (GDBusConnection *connection,
              const gchar     *name,
              gpointer         user_data)
{
  g_test_message ("Lost the name %s", name);
}


gint
main (gint argc, gchar *argv[])
{
  GMainLoop *loop;
  guint id;

  g_log_writer_default_set_use_stderr (TRUE);

  g_print ("Addre: %s\n", g_getenv ("DBUS_SESSION_BUS_ADDRESS"));

  loop = g_main_loop_new (NULL, FALSE);

  id = g_bus_own_name (G_BUS_TYPE_SESSION,
                       "org.freedesktop.portal.Desktop",
                       G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT |
                       G_BUS_NAME_OWNER_FLAGS_REPLACE,
                       on_bus_acquired,
                       on_name_acquired,
                       on_name_lost,
                       loop,
                       NULL);

  g_main_loop_run (loop);

  g_bus_unown_name (id);
  g_main_loop_unref (loop);

  return 0;
}
