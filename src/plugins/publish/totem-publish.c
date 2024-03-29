/* * -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2007 Openismus GmbH
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301  USA.
 *
 * The Totem project hereby grant permission for non-gpl compatible GStreamer
 * plugins to be used and distributed together with GStreamer and Totem. This
 * permission are above and beyond the permissions granted by the GPL license
 * Totem is covered by.
 *
 * See license_change file for details.
 *
 * Author:
 * 	Mathias Hasselmann
 *
 */

#include "config.h"

#include <glib-object.h>
#include <glib/gi18n-lib.h>
#include <gio/gio.h>

#include <libepc/consumer.h>
#include <libepc/enums.h>
#include <libepc/publisher.h>
#include <libepc/service-monitor.h>
#include <libepc-ui/progress-window.h>

#include <libpeas/peas-extension-base.h>
#include <libpeas/peas-object-module.h>
#include <libpeas/peas-activatable.h>
#include <libpeas-gtk/peas-gtk-configurable.h>

#include "totem-plugin.h"
#include "totem-private.h"
#include "totem-dirs.h"
#include "totem.h"

#define TOTEM_TYPE_PUBLISH_PLUGIN		(totem_publish_plugin_get_type ())
#define TOTEM_PUBLISH_PLUGIN(o)			(G_TYPE_CHECK_INSTANCE_CAST ((o), TOTEM_TYPE_PUBLISH_PLUGIN, TotemPublishPlugin))
#define TOTEM_PUBLISH_PLUGIN_CLASS(k)		(G_TYPE_CHECK_CLASS_CAST((k), TOTEM_TYPE_PUBLISH_PLUGIN, TotemPublishPluginClass))
#define TOTEM_IS_PUBLISH_PLUGIN(o)		(G_TYPE_CHECK_INSTANCE_TYPE ((o), TOTEM_TYPE_PUBLISH_PLUGIN))
#define TOTEM_IS_PUBLISH_PLUGIN_CLASS(k)	(G_TYPE_CHECK_CLASS_TYPE ((k), TOTEM_TYPE_PUBLISH_PLUGIN))
#define TOTEM_PUBLISH_PLUGIN_GET_CLASS(o)	(G_TYPE_INSTANCE_GET_CLASS ((o), TOTEM_TYPE_PUBLISH_PLUGIN, TotemPublishPluginClass))

#define TOTEM_PUBLISH_SCHEMA			TOTEM_GSETTINGS_SCHEMA ".plugins.publish"

enum
{
	NAME_COLUMN,
	INFO_COLUMN,
	LAST_COLUMN
};

typedef struct {
	TotemObject       *totem;
	GSettings         *gsettings;
	GtkWidget         *settings;
	GtkWidget         *scanning;
	GtkBuilder        *ui;

	EpcPublisher      *publisher;
	EpcServiceMonitor *monitor;
	GtkListStore      *neighbours;
	GSList            *playlist;

	guint scanning_id;

	gulong item_added_id;
	gulong item_removed_id;
} TotemPublishPluginPrivate;

TOTEM_PLUGIN_REGISTER_CONFIGURABLE (TOTEM_TYPE_PUBLISH_PLUGIN, TotemPublishPlugin, totem_publish_plugin);

void totem_publish_plugin_service_name_entry_changed_cb	   (GtkEntry        *entry,
							    TotemPublishPlugin *self);
void totem_publish_plugin_encryption_button_toggled_cb	   (GtkToggleButton *button,
							    TotemPublishPlugin *self);
void totem_publish_plugin_dialog_response_cb		   (GtkDialog       *dialog,
							    gint             response,
							    gpointer         data);
void totem_publish_plugin_neighbours_list_row_activated_cb (GtkTreeView       *view,
							    GtkTreePath       *path,
							    GtkTreeViewColumn *column,
							    gpointer           data);

G_LOCK_DEFINE_STATIC(totem_publish_plugin_lock);

static void
totem_publish_plugin_name_changed_cb (GSettings *settings, const gchar *key, TotemPublishPlugin *self)
{
	gchar *pattern, *name;

	pattern = g_settings_get_string (settings, "name-format");
	name = epc_publisher_expand_name (pattern, NULL);
	g_free (pattern);

	epc_publisher_set_service_name (self->priv->publisher, name);

	g_free (name);
}

void
totem_publish_plugin_service_name_entry_changed_cb (GtkEntry *entry, TotemPublishPlugin *self)
{
	g_settings_set_string (self->priv->gsettings, "name-format", gtk_entry_get_text (entry));
}

void
totem_publish_plugin_encryption_button_toggled_cb (GtkToggleButton *button, TotemPublishPlugin *self)
{
	g_settings_set_string (self->priv->gsettings, "protocol", gtk_toggle_button_get_active (button) ? "https" : "http");
}

static void
totem_publish_plugin_protocol_changed_cb (GSettings *settings, const gchar *key, TotemPublishPlugin *self)
{
	gchar *protocol_name;
	EpcProtocol protocol;
	GError *error = NULL;

	protocol_name = g_settings_get_string (settings, "protocol");
	protocol = epc_protocol_from_name (protocol_name, EPC_PROTOCOL_HTTPS);
	g_free (protocol_name);

	epc_publisher_quit (self->priv->publisher);
	epc_publisher_set_protocol (self->priv->publisher, protocol);
	epc_publisher_run_async (self->priv->publisher, &error);

	if (error) {
		g_warning ("%s: %s", G_STRFUNC, error->message);
		g_error_free (error);
	}
}

static gchar*
totem_publish_plugin_build_key (const gchar *filename)
{
	return g_strconcat ("media/", filename, NULL);
}

static EpcContents*
totem_publish_plugin_playlist_cb (EpcPublisher *publisher,
				  const gchar  *key,
				  gpointer      data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);
	GString *buffer = g_string_new (NULL);
	EpcContents *contents = NULL;
	GSList *iter;
	gint i;

	G_LOCK (totem_publish_plugin_lock);

	g_string_append_printf (buffer,
				"[playlist]\nNumberOfEntries=%d\n",
				g_slist_length (self->priv->playlist));

	for (iter = self->priv->playlist, i = 1; iter; iter = iter->next, ++i) {
		gchar *file_key = iter->data;
		gchar *uri;

		uri = epc_publisher_get_uri (publisher, file_key, NULL);

		g_string_append_printf (buffer,
					"File%d=%s\nTitle%d=%s\n",
					i, uri, i, file_key + 6);

		g_free (uri);
	}

	G_UNLOCK (totem_publish_plugin_lock);

	contents = epc_contents_new ("audio/x-scpls",
				     buffer->str, buffer->len,
				     g_free);

	g_string_free (buffer, FALSE);

	return contents;
}

static gboolean
totem_publish_plugin_stream_cb (EpcContents *contents,
				gpointer     buffer,
				gsize       *length,
				gpointer     data)
{
	GInputStream *stream = data;
	gssize size = 65536;

	g_return_val_if_fail (NULL != contents, FALSE);
	g_return_val_if_fail (NULL != length, FALSE);

	if (NULL == data || *length < (gsize)size) {
		*length = MAX (*length, (gsize)size);
		return FALSE;
	}

	size = g_input_stream_read (stream, buffer, size, NULL, NULL);
	if (size == -1) {
		g_input_stream_close (stream, NULL, NULL);
		size = 0;
	}

	*length = size;

	return size > 0;
}

static EpcContents*
totem_publish_plugin_media_cb (EpcPublisher *publisher,
			       const gchar  *key,
			       gpointer      data)
{
	GFileInputStream *stream;
	const gchar *url = data;
	GFile *file;

	file = g_file_new_for_uri (url);
	stream = g_file_read (file, NULL, NULL);
	g_object_unref (file);

	if (stream) {
		EpcContents *output = epc_contents_stream_new (
			NULL, totem_publish_plugin_stream_cb,
			stream, g_object_unref);

		return output;
	}

	return NULL;
}

static void
totem_publish_plugin_rebuild_playlist_cb (TotemPlaylist *playlist,
					  const gchar   *filename,
					  const gchar   *url,
					  gpointer       data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);
	gchar *key = totem_publish_plugin_build_key (filename);
	self->priv->playlist = g_slist_prepend (self->priv->playlist, key);
}

static void
totem_publish_plugin_playlist_changed_cb (TotemPlaylist *playlist,
					  gpointer       data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);

	G_LOCK (totem_publish_plugin_lock);

	g_slist_foreach (self->priv->playlist, (GFunc) g_free, NULL);
	g_slist_free (self->priv->playlist);
	self->priv->playlist = NULL;

	totem_playlist_foreach (playlist,
				totem_publish_plugin_rebuild_playlist_cb,
				self);

	self->priv->playlist = g_slist_reverse (self->priv->playlist);

	G_UNLOCK (totem_publish_plugin_lock);
}

static void
totem_publish_plugin_playlist_item_added_cb (TotemPlaylist *playlist,
					     const gchar   *filename,
					     const gchar   *url,
					     gpointer       data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);
	gchar *key = totem_publish_plugin_build_key (filename);

	epc_publisher_add_handler (self->priv->publisher, key,
				   totem_publish_plugin_media_cb,
				   g_strdup (url), g_free);

	g_free (key);

}

static void
totem_publish_plugin_playlist_item_removed_cb (TotemPlaylist *playlist,
				      const gchar   *filename,
				      const gchar   *url,
				      gpointer       data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);
	gchar *key = totem_publish_plugin_build_key (filename);
	epc_publisher_remove (self->priv->publisher, key);
	g_free (key);
}

static void
totem_publish_plugin_service_found_cb (TotemPublishPlugin *self,
				       const gchar        *name,
				       EpcServiceInfo     *info)
{
	GtkTreeIter iter;

	gtk_list_store_append (self->priv->neighbours, &iter);
	gtk_list_store_set (self->priv->neighbours, &iter, NAME_COLUMN, name,
						     INFO_COLUMN, info,
						     -1);
}

static void
totem_publish_plugin_service_removed_cb (TotemPublishPlugin *self,
					 const gchar        *name,
					 const gchar        *type)
{
	GtkTreeModel *model = GTK_TREE_MODEL (self->priv->neighbours);
	GtkTreeIter iter;

	if (gtk_tree_model_get_iter_first (model, &iter)) {
		GSList *path_list = NULL, *path_iter;
		GtkTreePath *path;
		gchar *stored;

		do {
			gtk_tree_model_get (model, &iter, NAME_COLUMN, &stored, -1);

			if (g_str_equal (stored, name)) {
				path = gtk_tree_model_get_path (model, &iter);
				path_list = g_slist_prepend (path_list, path);
			}
		} while (gtk_tree_model_iter_next (model, &iter));

		for (path_iter = path_list; path_iter; path_iter = path_iter->next) {
			path = path_iter->data;

			if (gtk_tree_model_get_iter (model, &iter, path))
				gtk_list_store_remove (self->priv->neighbours, &iter);

			gtk_tree_path_free (path);
		}

		g_slist_free (path_list);
	}
}

static void
totem_publish_plugin_scanning_done_cb (TotemPublishPlugin *self,
				       const gchar        *type)
{
	if (self->priv->scanning_id) {
		g_source_remove (self->priv->scanning_id);
		self->priv->scanning_id = 0;
	}

	if (self->priv->scanning)
		gtk_widget_hide (self->priv->scanning);
}

static void
totem_publish_plugin_load_playlist (TotemPublishPlugin   *self,
				    const EpcServiceInfo *info)
{
	EpcConsumer *consumer = epc_consumer_new (info);
	GKeyFile *keyfile = g_key_file_new ();
	gchar *contents = NULL;
	GError *error = NULL;
	gsize length = 0;

	contents = epc_consumer_lookup (consumer, "playlist.pls", &length, &error);

	if (contents && g_key_file_load_from_data (keyfile, contents, length, G_KEY_FILE_NONE, &error)) {
		gint i, n_entries;
		GList *mrl_list = NULL;

		/* returns zero in case of errors */
		n_entries = g_key_file_get_integer (keyfile, "playlist", "NumberOfEntries", &error);

		if (error)
			goto out;

		/* FIXME
		ev_sidebar_set_current_page (EV_SIDEBAR (self->priv->totem->sidebar), "playlist"); */
		totem_playlist_clear (self->priv->totem->playlist);

		for (i = 0; i < n_entries; ++i) {
			gchar *key, *mrl, *title;

			key = g_strdup_printf ("File%d", i + 1);
			mrl = g_key_file_get_string (keyfile, "playlist", key, NULL);
			g_free (key);

			key = g_strdup_printf ("Title%d", i + 1);
			title = g_key_file_get_string (keyfile, "playlist", key, NULL);
			g_free (key);

			if (mrl)
				mrl_list = g_list_prepend (mrl_list, totem_playlist_mrl_data_new (mrl, title));

			g_free (title);
			g_free (mrl);
		}

		/* Add the MRLs to the playlist asynchronously and in order */
		if (mrl_list != NULL)
			totem_playlist_add_mrls (self->priv->totem->playlist, g_list_reverse (mrl_list), FALSE, NULL, NULL, NULL);
	}

out:
	if (error) {
		g_warning ("Cannot load playlist: %s", error->message);
		g_error_free (error);
	}

	g_key_file_free (keyfile);
	g_free (contents);

	g_object_unref (consumer);
}

void
totem_publish_plugin_neighbours_list_row_activated_cb (GtkTreeView       *view,
						       GtkTreePath       *path,
						       GtkTreeViewColumn *column,
						       gpointer           data)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (data);
	EpcServiceInfo *info = NULL;
	GtkTreeIter iter;

	if (gtk_tree_model_get_iter (GTK_TREE_MODEL (self->priv->neighbours), &iter, path)) {
		gtk_tree_model_get (GTK_TREE_MODEL (self->priv->neighbours),
				    &iter, INFO_COLUMN, &info, -1);
		totem_publish_plugin_load_playlist (self, info);
		epc_service_info_unref (info);
	}
}

static gboolean
totem_publish_plugin_scanning_cb (gpointer data)
{
	gtk_progress_bar_pulse (GTK_PROGRESS_BAR (data));
	return TRUE;
}

static GtkWidget*
totem_publish_plugin_create_neigbours_page (TotemPublishPlugin *self, GtkBuilder *builder)
{
	GtkWidget *page, *list;

	page = GTK_WIDGET (gtk_builder_get_object (builder, "neighbours-page"));
	list = GTK_WIDGET (gtk_builder_get_object (builder, "neighbours-list"));

	self->priv->scanning = GTK_WIDGET (gtk_builder_get_object (builder, "neighbours-scanning"));
	self->priv->scanning_id = g_timeout_add (100, totem_publish_plugin_scanning_cb, self->priv->scanning);

	g_signal_connect_swapped (self->priv->monitor, "service-found",
				  G_CALLBACK (totem_publish_plugin_service_found_cb),
				  self);
	g_signal_connect_swapped (self->priv->monitor, "service-removed",
				  G_CALLBACK (totem_publish_plugin_service_removed_cb),
				  self);
	g_signal_connect_swapped (self->priv->monitor, "scanning-done",
				  G_CALLBACK (totem_publish_plugin_scanning_done_cb),
				  self);

	self->priv->neighbours = gtk_list_store_new (LAST_COLUMN, G_TYPE_STRING, EPC_TYPE_SERVICE_INFO);

	gtk_tree_view_set_model (GTK_TREE_VIEW (list),
				 GTK_TREE_MODEL (self->priv->neighbours));

	gtk_tree_view_append_column (GTK_TREE_VIEW (list),
		gtk_tree_view_column_new_with_attributes (
			NULL, gtk_cell_renderer_text_new (),
			"text", NAME_COLUMN, NULL));

	g_object_ref (page);
	gtk_widget_unparent (page);

	return page;
}

static void
impl_activate (PeasActivatable *plugin)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (plugin);
	TotemPublishPluginPrivate *priv = self->priv;
	EpcProtocol protocol = EPC_PROTOCOL_HTTPS;
	GtkWindow *window;
	GtkBuilder *builder;
	GError *internal_error = NULL;

	gchar *protocol_name;

	gchar *service_pattern;
	gchar *service_name;

	g_return_if_fail (NULL == priv->publisher);
	g_return_if_fail (NULL == priv->totem);

	G_LOCK (totem_publish_plugin_lock);

	priv->totem = g_object_ref (g_object_get_data (G_OBJECT (plugin), "object"));

	window = totem_get_main_window (priv->totem);
	builder = totem_plugin_load_interface ("publish", "publish-plugin.ui", TRUE, window, self);
	epc_progress_window_install (window);
	g_object_unref (window);

	priv->gsettings = g_settings_new (TOTEM_PUBLISH_SCHEMA);

	protocol_name = g_settings_get_string (priv->gsettings, "protocol");
	service_pattern = g_settings_get_string (priv->gsettings, "name-format");

	if (*protocol_name == '\0') {
		protocol_name = g_strdup ("http");
		g_settings_set_string (priv->gsettings, "protocol", protocol_name);
	}

	if (*service_pattern == '\0') {
		service_pattern = g_strdup ("%a of %u on %h");
		g_settings_set_string (priv->gsettings, "name-format", service_pattern);
	}

	g_signal_connect (priv->gsettings, "changed::name", (GCallback) totem_publish_plugin_name_changed_cb, self);
	g_signal_connect (priv->gsettings, "changed::protocol", (GCallback) totem_publish_plugin_protocol_changed_cb, self);

	protocol = epc_protocol_from_name (protocol_name, EPC_PROTOCOL_HTTPS);
	service_name = epc_publisher_expand_name (service_pattern, &internal_error);
	g_free (service_pattern);

	if (internal_error) {
		g_warning ("%s: %s", G_STRFUNC, internal_error->message);
		g_clear_error (&internal_error);
	}

	priv->monitor = epc_service_monitor_new ("totem", NULL, EPC_PROTOCOL_UNKNOWN);
	epc_service_monitor_set_skip_our_own (priv->monitor, TRUE);

	/* Translators: computers on the local network which are publishing their playlists over the network */
	totem_add_sidebar_page (priv->totem, "neighbours", _("Neighbors"),
				totem_publish_plugin_create_neigbours_page (self, builder));
	g_object_unref (builder);

	priv->publisher = epc_publisher_new (service_name, "totem", NULL);
	epc_publisher_set_protocol (priv->publisher, protocol);

	g_free (protocol_name);
	g_free (service_name);

	epc_publisher_add_handler (priv->publisher, "playlist.pls",
				   totem_publish_plugin_playlist_cb,
				   self, NULL);
	epc_publisher_add_bookmark (priv->publisher, "playlist.pls", NULL);

	priv->item_added_id = g_signal_connect (priv->totem->playlist, "changed",
		G_CALLBACK (totem_publish_plugin_playlist_changed_cb), self);
	priv->item_added_id = g_signal_connect (priv->totem->playlist, "item-added",
		G_CALLBACK (totem_publish_plugin_playlist_item_added_cb), self);
	priv->item_removed_id = g_signal_connect (priv->totem->playlist, "item-removed",
		G_CALLBACK (totem_publish_plugin_playlist_item_removed_cb), self);

	G_UNLOCK (totem_publish_plugin_lock);

	totem_playlist_foreach (priv->totem->playlist,
				totem_publish_plugin_playlist_item_added_cb, self);

	totem_publish_plugin_playlist_changed_cb (priv->totem->playlist, self);

	epc_publisher_run_async (priv->publisher, NULL);

	return;
}

static void
impl_deactivate (PeasActivatable *plugin)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (plugin);
	TotemPublishPluginPrivate *priv = self->priv;
	TotemPlaylist *playlist = NULL;

	G_LOCK (totem_publish_plugin_lock);

	if (priv->totem)
		playlist = priv->totem->playlist;

	if (priv->scanning_id) {
		g_source_remove (priv->scanning_id);
		priv->scanning_id = 0;
	}

	if (playlist && priv->item_added_id) {
		g_signal_handler_disconnect (playlist, priv->item_added_id);
		priv->item_added_id = 0;
	}

	if (playlist && priv->item_removed_id) {
		g_signal_handler_disconnect (playlist, priv->item_removed_id);
		priv->item_removed_id = 0;
	}

	if (priv->monitor) {
		g_object_unref (priv->monitor);
		priv->monitor = NULL;
	}

	if (priv->publisher) {
		epc_publisher_quit (priv->publisher);
		g_object_unref (priv->publisher);
		priv->publisher = NULL;
	}

	if (priv->gsettings != NULL)
		g_object_unref (priv->gsettings);
	priv->gsettings = NULL;

	if (priv->totem) {
		totem_remove_sidebar_page (priv->totem, "neighbours");

		g_object_unref (priv->totem);
		priv->totem = NULL;
	}

	if (priv->settings) {
		gtk_widget_destroy (priv->settings);
		priv->settings = NULL;
	}

	if (priv->playlist) {
		g_slist_foreach (priv->playlist, (GFunc) g_free, NULL);
		g_slist_free (priv->playlist);
		priv->playlist = NULL;
	}

	G_UNLOCK (totem_publish_plugin_lock);

	priv->scanning = NULL;
}

static GtkWidget *
impl_create_configure_widget (PeasGtkConfigurable *configurable)
{
	TotemPublishPlugin *self = TOTEM_PUBLISH_PLUGIN (configurable);
	gchar *service_name, *protocol_name;
	GtkBuilder *builder;
	GtkWidget *widget;
	EpcProtocol protocol;
	GSettings *settings;

	/* This function has to be independent of the rest of the plugin, as it's executed under a different plugin instance.
	 * FIXME: bgo#624073 */
	builder = totem_plugin_load_interface ("publish", "publish-plugin.ui", TRUE, NULL, self);

	settings = g_settings_new (TOTEM_PUBLISH_SCHEMA);
	service_name = g_settings_get_string (settings, "name-format");
	protocol_name = g_settings_get_string (settings, "protocol");
	g_object_unref (settings);

	protocol = epc_protocol_from_name (protocol_name, EPC_PROTOCOL_HTTPS);
	g_free (protocol_name);

	widget = g_object_ref (gtk_builder_get_object (builder, "publish-settings-vbox"));

	gtk_entry_set_text (GTK_ENTRY (gtk_builder_get_object (builder, "service-name-entry")), service_name);
	gtk_toggle_button_set_active (GTK_TOGGLE_BUTTON (gtk_builder_get_object (builder, "encryption-button")), EPC_PROTOCOL_HTTPS == protocol);

	g_free (service_name);

	return widget;
}
