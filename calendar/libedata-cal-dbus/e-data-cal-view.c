/* Evolution calendar - Live search query implementation
 *
 * Copyright (C) 2001 Ximian, Inc.
 *
 * Author: Federico Mena-Quintero <federico@ximian.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <string.h>
#include <glib.h>

/* DBus includes */
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <glib-object.h>
/**/

#include "e-cal-backend-sexp.h"
#include "e-data-cal-view.h"

extern DBusGConnection *connection;

static gboolean impl_EDataCalView_start (EDataCalView *query, GError **error);
#include "e-data-cal-view-glue.h"
#include "e-data-cal-view-listener-bindings.h"

/* Generate the GObject boilerplate */
G_DEFINE_TYPE(EDataCalView, e_data_cal_view, G_TYPE_OBJECT)

/* Private part of the Query structure */
struct _EDataCalViewPrivate {
	/* The backend we are monitoring */
	ECalBackend *backend;

	gboolean started;
	gboolean done;
	EDataCalCallStatus done_status;

	GHashTable *matched_objects;

	/* The listener we report to */
	GList *listeners;

	/* Sexp that defines the query */
	ECalBackendSExp *sexp;
	
	GMutex *notification_mutex;
};

#define E_DATA_CAL_VIEW_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_DATA_CAL_VIEW_TYPE, EDataCalViewPrivate))

static void e_data_cal_view_dispose (GObject *object);
static void e_data_cal_view_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec);
static void e_data_cal_view_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec);
static void listener_died_cb (DBusGProxy *proxy, EDataCalView *query);
static GObjectClass *parent_class;

typedef struct {
	DBusGProxy *listener;

	gboolean notified_start;
	gboolean notified_done;
} ListenerData;


/* Property IDs */
enum props {
	PROP_0,
	PROP_BACKEND,
	PROP_LISTENER,
	PROP_SEXP
};


/* Class init */
static void
e_data_cal_view_class_init (EDataCalViewClass *klass)
{
	GParamSpec *param;
	GObjectClass *object_class = G_OBJECT_CLASS (klass);
	
	parent_class = g_type_class_peek_parent (klass);
	
	object_class->set_property = e_data_cal_view_set_property;
	object_class->get_property = e_data_cal_view_get_property;
	object_class->dispose = e_data_cal_view_dispose;
	g_type_class_add_private (klass, sizeof (EDataCalViewPrivate));

	param =  g_param_spec_object ("backend", NULL, NULL, E_TYPE_CAL_BACKEND,
				      G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
	g_object_class_install_property (object_class, PROP_BACKEND, param);
	param =  g_param_spec_pointer ("listener", NULL, NULL,
				      G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
	g_object_class_install_property (object_class, PROP_LISTENER, param);
	param =  g_param_spec_object ("sexp", NULL, NULL, E_TYPE_CAL_BACKEND_SEXP,
				      G_PARAM_READABLE | G_PARAM_WRITABLE | G_PARAM_CONSTRUCT_ONLY);
	g_object_class_install_property (object_class, PROP_SEXP, param);
	
	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass), &dbus_glib_e_data_cal_view_object_info);
}

/* Instance init */
static void
e_data_cal_view_init (EDataCalView *query)
{
	EDataCalViewPrivate *priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	priv->backend = NULL;
	priv->started = FALSE;
	priv->done = FALSE;
	priv->done_status = Success;
	priv->matched_objects = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, g_free);
	priv->listeners = NULL;
	priv->started = FALSE;
	priv->sexp = NULL;
	priv->notification_mutex = g_mutex_new ();
}

EDataCalView *
e_data_cal_view_new (ECalBackend *backend,
		     DBusGProxy *ql, char *path,
		     ECalBackendSExp *sexp)
{
	EDataCalView *query;

	query = g_object_new (E_DATA_CAL_VIEW_TYPE, "backend", backend, "listener", ql, "sexp", sexp, NULL);
	query->path = path;
	
	return query;
}

static void
add_object_to_cache (EDataCalView *query, const char *calobj)
{
	ECalComponent *comp;
	char *real_uid;
	const char *uid;
	EDataCalViewPrivate *priv;

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	comp = e_cal_component_new_from_string (calobj);
	if (!comp)
		return;

	e_cal_component_get_uid (comp, &uid);
	if (!uid || !*uid) {
		g_object_unref (comp);
		return;
	}

	if (e_cal_component_is_instance (comp))
		real_uid = g_strdup_printf ("%s@%s", uid, e_cal_component_get_recurid_as_string (comp));
	else
		real_uid = g_strdup (uid);

	if (g_hash_table_lookup (priv->matched_objects, real_uid))
		g_hash_table_replace (priv->matched_objects, real_uid, g_strdup (calobj));
	else
		g_hash_table_insert (priv->matched_objects, real_uid, g_strdup (calobj));

	/* free memory */
	g_object_unref (comp);
}

static gboolean
uncache_with_uid_cb (gpointer key, gpointer value, gpointer user_data)
{
	ECalComponent *comp;
	const char *this_uid;
	char *uid, *object;

	uid = user_data;
	object = value;

	comp = e_cal_component_new_from_string (object);
	if (comp) {
		e_cal_component_get_uid (comp, &this_uid);
		if (this_uid && *this_uid && !strcmp (uid, this_uid)) {
			g_object_unref (comp);
			return TRUE;
		}

		g_object_unref (comp);
	}

	return FALSE;
}

static void
remove_object_from_cache (EDataCalView *query, const char *uid)
{
	EDataCalViewPrivate *priv;

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	g_hash_table_foreach_remove (priv->matched_objects, (GHRFunc) uncache_with_uid_cb, (gpointer) uid);
}

static void
listener_died_cb (DBusGProxy *proxy, EDataCalView *query)
{
	EDataCalViewPrivate *priv;
	GList *l;

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
/*	g_warning ("%s, %s: Received listener-died signal", G_STRLOC, __FUNCTION__);*/

	g_mutex_lock (priv->notification_mutex);
	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;

		if (ld->listener == proxy) {
/*			g_warning ("%s, %s: Removing listener", G_STRLOC, __FUNCTION__);*/
			g_object_unref (ld->listener);
			ld->listener = NULL;

			priv->listeners = g_list_remove_link (priv->listeners, l);
			g_list_free (l);
			g_free (ld);
			break;
		}
	}
	g_mutex_unlock (priv->notification_mutex);
}

static void
generic_reply (DBusGProxy *proxy, GError *error, gpointer user_data)
{
	if (error) {
		g_printerr("%s: %s\n", user_data ? (char*)user_data : __FUNCTION__, error->message);
		g_error_free (error);
	}
}

typedef struct {
	EDataCalView *query;
	DBusGProxy *proxy;
	char **obj_list;
} ObjectsAddedData;

static gboolean
notify_objects_added_idle (gpointer d)
{
	ObjectsAddedData *data = d;
	EDataCalViewPrivate *priv;
	
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (data->query);
	
	g_mutex_lock (priv->notification_mutex);
	if (!org_gnome_evolution_dataserver_calendar_DataCalViewListener_notify_objects_added_async (data->proxy, (const char**)data->obj_list, generic_reply, (void*)__FUNCTION__)) {
		g_warning (G_STRLOC ": notifyObjectsAdded failed");
	}
	g_mutex_unlock (priv->notification_mutex);
	
	g_strfreev (data->obj_list);
	g_free (data);
	return FALSE;
}

static void
notify_matched_object_cb (gpointer key, gpointer value, gpointer user_data)
{
	char *uid, *object;
	EDataCalView *query;
	EDataCalViewPrivate *priv;
	GList *l;

	uid = key;
	object = value;
	query = user_data;
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;

		if (!ld->notified_start) {
			ObjectsAddedData *data = g_new (ObjectsAddedData, 1);
			char **obj_list = g_new0 (char *, 2);

			obj_list[0] = g_strdup (object);

			data->query = query;
			data->proxy = ld->listener;
			data->obj_list = obj_list;
			g_idle_add (notify_objects_added_idle, data);
		}
	}
}

typedef struct {
	EDataCalView *query;
	DBusGProxy *proxy;
	gboolean *notified_done;
} QueryDoneData;

static gboolean
notify_query_done_idle (gpointer d)
{
	QueryDoneData *data = d;
	EDataCalViewPrivate *priv;

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (data->query);
	
	g_mutex_lock (priv->notification_mutex);
	if (!org_gnome_evolution_dataserver_calendar_DataCalViewListener_notify_query_done_async (data->proxy, priv->done_status, generic_reply, (void*)__FUNCTION__)) {
		g_warning (G_STRLOC ": notifyQueryDone failed");
	}
		else if (data->notified_done) *(data->notified_done) = TRUE;
	g_mutex_unlock (priv->notification_mutex);
	
	g_free (data);
	return FALSE;
}

static gboolean
impl_EDataCalView_start (EDataCalView *query, GError **error)
{
	EDataCalViewPrivate *priv;
	GList *l;

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
	g_mutex_lock (priv->notification_mutex);

	if (priv->started) {
		g_hash_table_foreach (priv->matched_objects, (GHFunc) notify_matched_object_cb, query);

		/* notify all listeners correctly if the query is already done */
		for (l = priv->listeners; l != NULL; l = l->next) {
			ListenerData *ld = l->data;

			if (!ld->notified_start) {
				ld->notified_start = TRUE;

				if (priv->done && !ld->notified_done) {
					QueryDoneData *data = g_new (QueryDoneData, 1);
					
					data->query = query;
					data->proxy = ld->listener;
					data->notified_done = &ld->notified_done;
					g_idle_add (notify_query_done_idle, data);
				}
			}
		}
	} else {
		priv->started = TRUE;
		e_cal_backend_start_query (priv->backend, query);

		for (l = priv->listeners; l != NULL; l = l->next) {
			ListenerData *ld = l->data;
			ld->notified_start = TRUE;
		}
	}
	
	g_mutex_unlock (priv->notification_mutex);
	
	return TRUE;
}

static void
e_data_cal_view_set_property (GObject *object, guint property_id, const GValue *value, GParamSpec *pspec)
{
	EDataCalView *query;
	EDataCalViewPrivate *priv;

	query = QUERY (object);
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
	
	switch (property_id) {
	case PROP_BACKEND:
		priv->backend = E_CAL_BACKEND (g_value_dup_object (value));
		break;
	case PROP_LISTENER:
		e_data_cal_view_add_listener (query, g_value_get_pointer (value));
		break;
	case PROP_SEXP:
		priv->sexp = E_CAL_BACKEND_SEXP (g_value_dup_object (value));
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
e_data_cal_view_get_property (GObject *object, guint property_id, GValue *value, GParamSpec *pspec)
{
	EDataCalView *query;
	EDataCalViewPrivate *priv;
	
	query = QUERY (object);
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	switch (property_id) {
	case PROP_BACKEND:
		g_value_set_object (value, priv->backend);
	case PROP_LISTENER:

		if (priv->listeners) {
			ListenerData *ld;

			ld = priv->listeners->data;
			g_value_set_pointer (value, ld->listener);
		}
		break;
	case PROP_SEXP:
		g_value_set_object (value, priv->sexp);
		break;
	default:
		G_OBJECT_WARN_INVALID_PROPERTY_ID (object, property_id, pspec);
		break;
	}
}

static void
e_data_cal_view_dispose (GObject *object)
{
	EDataCalView *query;
	EDataCalViewPrivate *priv;

	g_return_if_fail (object != NULL);
	g_return_if_fail (IS_QUERY (object));

	query = QUERY (object);
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	if (priv->backend)
		g_object_unref (priv->backend);

	while (priv->listeners) {
		ListenerData *ld = priv->listeners->data;

		if (ld->listener)
			g_object_unref (ld->listener);
		priv->listeners = g_list_remove (priv->listeners, ld);
		g_free (ld);
	}

	if (priv->matched_objects)
		g_hash_table_destroy (priv->matched_objects);

	if (priv->sexp)
		g_object_unref (priv->sexp);

	g_mutex_free (priv->notification_mutex);

	if (G_OBJECT_CLASS (parent_class)->dispose)
		(* G_OBJECT_CLASS (parent_class)->dispose) (object);
}

void
e_data_cal_view_add_listener (EDataCalView *query, DBusGProxy *ql)
{
	ListenerData *ld;
	EDataCalViewPrivate *priv;

	g_return_if_fail (IS_QUERY (query));
	g_return_if_fail (DBUS_IS_G_PROXY (ql));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	ld = g_new0 (ListenerData, 1);
	ld->listener = g_object_ref (ql);
	
	g_mutex_lock (priv->notification_mutex);
	priv->listeners = g_list_prepend (priv->listeners, ld);

/*	g_warning ("%s, %s: Adding listener_died signal", G_STRLOC, __FUNCTION__);*/
	dbus_g_proxy_add_signal (ql, "ListenerDied", G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (ql, "ListenerDied", G_CALLBACK (listener_died_cb), query, NULL);
	g_mutex_unlock (priv->notification_mutex);
}

/**
 * e_data_cal_view_get_text:
 * @query: A #EDataCalView object.
 *
 * Get the expression used for the given query.
 *
 * Return value: the query expression used to search.
 */
const char *
e_data_cal_view_get_text (EDataCalView *query)
{
	EDataCalViewPrivate *priv;
	g_return_val_if_fail (IS_QUERY (query), NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
	return e_cal_backend_sexp_text (priv->sexp);
}

/**
 * e_data_cal_view_get_object_sexp:
 * @query: A query object.
 *
 * Get the #ECalBackendSExp object used for the given query.
 *
 * Return value: The expression object used to search.
 */
ECalBackendSExp *
e_data_cal_view_get_object_sexp (EDataCalView *query)
{
	EDataCalViewPrivate *priv;
	g_return_val_if_fail (IS_QUERY (query), NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
	return priv->sexp;
}

/**
 * e_data_cal_view_object_matches:
 * @query: A query object.
 * @object: Object to match.
 *
 * Compares the given @object to the regular expression used for the
 * given query.
 *
 * Return value: TRUE if the object matches the expression, FALSE if not.
 */
gboolean
e_data_cal_view_object_matches (EDataCalView *query, const char *object)
{
	EDataCalViewPrivate *priv;
	
	g_return_val_if_fail (query != NULL, FALSE);
	g_return_val_if_fail (IS_QUERY (query), FALSE);
	g_return_val_if_fail (object != NULL, FALSE);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);
	
	return e_cal_backend_sexp_match_object (priv->sexp, object, priv->backend);
}

static void
add_object_to_list (gpointer key, gpointer value, gpointer user_data)
{
	GList **list = user_data;

	*list = g_list_append (*list, value);
}

/**
 * e_data_cal_view_get_matched_objects:
 * @query: A query object.
 *
 * Gets the list of objects already matched for the given query.
 *
 * Return value: A list of matched objects.
 */
GList *
e_data_cal_view_get_matched_objects (EDataCalView *query)
{
	EDataCalViewPrivate *priv;
	GList *list = NULL;

	g_return_val_if_fail (IS_QUERY (query), NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	g_hash_table_foreach (priv->matched_objects, (GHFunc) add_object_to_list, &list);

	return list;
}

/**
 * e_data_cal_view_is_started:
 * @query: A query object.
 *
 * Checks whether the given query has already been started.
 *
 * Return value: TRUE if the query has already been started, FALSE otherwise.
 */
gboolean
e_data_cal_view_is_started (EDataCalView *query)
{
	EDataCalViewPrivate *priv;

	g_return_val_if_fail (IS_QUERY (query), FALSE);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	return priv->started;
}

/**
 * e_data_cal_view_is_done:
 * @query: A query object.
 *
 * Checks whether the given query is already done. Being done means the initial
 * matching of objects have been finished, not that no more notifications about
 * changes will be sent. In fact, even after done, notifications will still be sent
 * if there are changes in the objects matching the query search expression.
 *
 * Return value: TRUE if the query is done, FALSE if still in progress.
 */
gboolean
e_data_cal_view_is_done (EDataCalView *query)
{
	EDataCalViewPrivate *priv;

	g_return_val_if_fail (IS_QUERY (query), FALSE);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	return priv->done;
}

/**
 * e_data_cal_view_get_done_status:
 * @query: A query object.
 *
 * Gets the status code obtained when the initial matching of objects was done
 * for the given query.
 *
 * Return value: The query status.
 */
EDataCalCallStatus
e_data_cal_view_get_done_status (EDataCalView *query)
{
	EDataCalViewPrivate *priv;

	g_return_val_if_fail (IS_QUERY (query), FALSE);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	if (priv->done)
		return priv->done_status;

	return Success;
}

/**
 * e_data_cal_view_notify_objects_added:
 * @query: A query object.
 * @objects: List of objects that have been added.
 *
 * Notifies all query listeners of the addition of a list of objects.
 */
void
e_data_cal_view_notify_objects_added (EDataCalView *query, const GList *objects)
{
	EDataCalViewPrivate *priv;
	char **obj_list;
	const GList *l;
	int num_objs, i;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	num_objs = g_list_length ((GList*)objects);
	if (num_objs <= 0)
		return;

	obj_list = g_new0 (char *, num_objs + 1);
	if (obj_list)
		for (l = objects, i = 0; l; l = l->next, i++) {
			obj_list[i] = g_strdup (l->data);	
			/* update our cache */
			add_object_to_cache (query, l->data);
		}

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;
		ObjectsAddedData *data = g_new (ObjectsAddedData, 1);

		data->query = query;
		data->proxy = ld->listener;
		data->obj_list = g_strdupv (obj_list);
		g_idle_add (notify_objects_added_idle, data);
	}

	g_strfreev (obj_list);
}

/**
 * e_data_cal_view_notify_objects_added_1:
 * @query: A query object.
 * @object: The object that has been added.
 *
 * Notifies all the query listeners of the addition of a single object.
 */
void
e_data_cal_view_notify_objects_added_1 (EDataCalView *query, const char *object)
{
	EDataCalViewPrivate *priv;
	GList objects;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));
	g_return_if_fail (object != NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	objects.next = objects.prev = NULL;
	objects.data = (gpointer)object;

	e_data_cal_view_notify_objects_added (query, &objects);
}

typedef struct {
	EDataCalView *query;
	DBusGProxy *proxy;
	char **obj_list;
} ObjectsModifiedData;

static gboolean
notify_objects_modified_idle (gpointer d)
{
	ObjectsModifiedData *data = d;
	EDataCalViewPrivate *priv;
	
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (data->query);
	
	g_mutex_lock (priv->notification_mutex);
	if (!org_gnome_evolution_dataserver_calendar_DataCalViewListener_notify_objects_modified_async (data->proxy, (const char**)data->obj_list, generic_reply, (void*)__FUNCTION__)) {
		g_warning (G_STRLOC ": notifyObjectsModified failed");
	}
	g_mutex_unlock (priv->notification_mutex);
	
	g_strfreev (data->obj_list);
	g_free (data);
	return FALSE;
}

/**
 * e_data_cal_view_notify_objects_modified:
 * @query: A query object.
 * @objects: List of modified objects.
 *
 * Notifies all query listeners of the modification of a list of objects.
 */
void
e_data_cal_view_notify_objects_modified (EDataCalView *query, const GList *objects)
{
	EDataCalViewPrivate *priv;
	char **obj_list;
	const GList *l;
	int num_objs, i;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	num_objs = g_list_length ((GList*)objects);
	if (num_objs <= 0)
		return;

	obj_list = g_new0 (char *, num_objs + 1);
	if (obj_list)
		for (l = objects, i = 0; l; l = l->next, i++) {
			obj_list[i] = g_strdup (l->data);
			/* update our cache */
			add_object_to_cache (query, l->data);
		}

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;
		ObjectsModifiedData *data = g_new (ObjectsModifiedData, 1);

		data->query = query;
		data->proxy = ld->listener;
		data->obj_list = g_strdupv (obj_list);
		g_idle_add (notify_objects_modified_idle, data);
	}

	g_strfreev (obj_list);
}

/**
 * e_data_cal_view_notify_objects_modified_1:
 * @query: A query object.
 * @object: The modified object.
 *
 * Notifies all query listeners of the modification of a single object.
 */
void
e_data_cal_view_notify_objects_modified_1 (EDataCalView *query, const char *object)
{
	EDataCalViewPrivate *priv;
	GList objects;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));
	g_return_if_fail (object != NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	objects.next = objects.prev = NULL;
	objects.data = (gpointer)object;
	
	e_data_cal_view_notify_objects_modified (query, &objects);
}

typedef struct {
	EDataCalView *query;
	DBusGProxy *proxy;
	char **uid_list;
} ObjectsRemovedData;

static gboolean
notify_objects_removed_idle (gpointer d)
{
	ObjectsRemovedData *data = d;
	EDataCalViewPrivate *priv;
	
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (data->query);
	
	g_mutex_lock (priv->notification_mutex);
	if (!org_gnome_evolution_dataserver_calendar_DataCalViewListener_notify_objects_removed_async (data->proxy, (const char**)data->uid_list, generic_reply, (void*)__FUNCTION__)) {
		g_warning (G_STRLOC ": notifyObjectsRemoved failed");
	}
	g_mutex_unlock (priv->notification_mutex);
	
	g_strfreev (data->uid_list);
	g_free (data);
	return FALSE;
}

/**
 * e_data_cal_view_notify_objects_removed:
 * @query: A query object.
 * @uids: List of UIDs for the objects that have been removed.
 *
 * Notifies all query listener of the removal of a list of objects.
 */
void
e_data_cal_view_notify_objects_removed (EDataCalView *query, const GList *uids)
{
	EDataCalViewPrivate *priv;
	char **uid_list;
	const GList *l;
	int num_uids, i;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	num_uids = g_list_length ((GList*)uids);
	if (num_uids <= 0)
		return;

	uid_list = g_new0 (char *, num_uids + 1);
	if (uid_list)
		for (l = uids, i = 0; l; l = l->next, i ++) {
			uid_list[i] = g_strdup (l->data);
			/* update our cache */
			remove_object_from_cache (query, l->data);
		}

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;
		ObjectsRemovedData *data = g_new (ObjectsRemovedData, 1);

		data->query = query;
		data->proxy = ld->listener;
		data->uid_list = g_strdupv (uid_list);
		g_idle_add (notify_objects_removed_idle, data);
	}

	g_strfreev (uid_list);
}

/**
 * e_data_cal_view_notify_objects_removed:
 * @query: A query object.
 * @uid: UID of the removed object.
 *
 * Notifies all query listener of the removal of a single object.
 */
void
e_data_cal_view_notify_objects_removed_1 (EDataCalView *query, const char *uid)
{
	EDataCalViewPrivate *priv;
	GList uids;
	
	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));
	g_return_if_fail (uid != NULL);

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	uids.next = uids.prev = NULL;
	uids.data = (gpointer)uid;
	
	e_data_cal_view_notify_objects_removed (query, &uids);
}

typedef struct {
	EDataCalView *query;
	DBusGProxy *proxy;
	char *message;
	int percent;
} QueryProgressData;

static gboolean
notify_query_progress_idle (gpointer d)
{
	QueryProgressData *data = d;
	EDataCalViewPrivate *priv;
	
	priv = E_DATA_CAL_VIEW_GET_PRIVATE (data->query);
	
	g_mutex_lock (priv->notification_mutex);
	if (!org_gnome_evolution_dataserver_calendar_DataCalViewListener_notify_query_progress_async (data->proxy, data->message, data->percent, generic_reply, (void*)__FUNCTION__)) {
		g_warning (G_STRLOC ": notifyQueryProgress failed");
	}
	g_mutex_unlock (priv->notification_mutex);
	
	g_free (data->message);
	g_free (data);
	return FALSE;
}

/**
 * e_data_cal_view_notify_progress:
 * @query: A query object.
 * @message: Progress message to send to listeners.
 * @percent: Percentage completed.
 *
 * Notifies all query listeners of progress messages.
 */
void
e_data_cal_view_notify_progress (EDataCalView *query, const char *message, int percent)
{
	EDataCalViewPrivate *priv;	
	GList *l;

	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;
		QueryProgressData *data = g_new (QueryProgressData, 1);

		data->query = query;
		data->proxy = ld->listener;
		data->message = g_strdup (message);
		data->percent = percent;
		g_idle_add (notify_query_progress_idle, data);
	}
}

/**
 * e_data_cal_view_notify_done:
 * @query: A query object.
 * @status: Query completion status code.
 *
 * Notifies all query listeners of the completion of the query, including a
 * status code.
 */
void
e_data_cal_view_notify_done (EDataCalView *query, GNOME_Evolution_Calendar_CallStatus status)
{
	EDataCalViewPrivate *priv;	
	GList *l;

	g_return_if_fail (query != NULL);
	g_return_if_fail (IS_QUERY (query));

	priv = E_DATA_CAL_VIEW_GET_PRIVATE (query);

	priv->done = TRUE;
	priv->done_status = status;

	for (l = priv->listeners; l != NULL; l = l->next) {
		ListenerData *ld = l->data;
		QueryDoneData *data = g_new0 (QueryDoneData, 1);

		data->query = query;
		data->proxy = ld->listener;
		g_idle_add (notify_query_done_idle, data);
	}
}
