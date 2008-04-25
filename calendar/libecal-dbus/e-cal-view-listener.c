/* Evolution calendar - Live search query listener convenience object
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

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include "e-cal-marshal.h"
#include "e-cal-view-listener.h"
#include "libedata-cal-dbus/e-data-cal-types.h"

static gboolean impl_notifyObjectsAdded (ECalViewListener *ql, char **objects, DBusGMethodInvocation *context);
static gboolean impl_notifyObjectsModified (ECalViewListener *ql, char **objects, DBusGMethodInvocation *context);
static gboolean impl_notifyObjectsRemoved (ECalViewListener *ql, char **uids, DBusGMethodInvocation *context);
static gboolean impl_notifyQueryProgress (ECalViewListener *ql, gchar *message, gint16 percent, DBusGMethodInvocation *context);
static gboolean impl_notifyQueryDone (ECalViewListener *ql, const EDataCalCallStatus status, DBusGMethodInvocation *context);
#include "e-data-cal-view-listener-glue.h"

G_DEFINE_TYPE(ECalViewListener, e_cal_view_listener, G_TYPE_OBJECT);

/* Private part of the CalViewListener structure */

struct _ECalViewListenerPrivate {
	int dummy;
};

#define E_CAL_VIEW_LISTENER_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_CAL_VIEW_LISTENER, ECalViewListenerPrivate))

/* Signal IDs */
enum {
	OBJECTS_ADDED,
	OBJECTS_MODIFIED,
	OBJECTS_REMOVED,
	VIEW_PROGRESS,
	VIEW_DONE,
	LISTENER_DIED,
	LAST_SIGNAL
};
static guint signals[LAST_SIGNAL] = { 0 };

static GObjectClass *parent_class;

static void e_cal_view_listener_dispose (GObject *object);

static void
e_cal_view_listener_class_init (ECalViewListenerClass *klass)
{
	GObjectClass *object_class;

	object_class = (GObjectClass *) klass;
	
	object_class->dispose = e_cal_view_listener_dispose;

	parent_class = g_type_class_peek_parent (klass);
	g_type_class_add_private (klass, sizeof (ECalViewListenerPrivate));
	
	signals[OBJECTS_ADDED] =
		g_signal_new ("objects_added",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (ECalViewListenerClass, objects_added),
			      NULL, NULL,
			      e_cal_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[OBJECTS_MODIFIED] =
		g_signal_new ("objects_modified",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (ECalViewListenerClass, objects_modified),
			      NULL, NULL,
			      e_cal_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[OBJECTS_REMOVED] =
		g_signal_new ("objects_removed",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (ECalViewListenerClass, objects_removed),
			      NULL, NULL,
			      e_cal_marshal_VOID__POINTER,
			      G_TYPE_NONE, 1, G_TYPE_POINTER);
	signals[VIEW_PROGRESS] =
		g_signal_new ("view_progress",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (ECalViewListenerClass, view_progress),
			      NULL, NULL,
			      e_cal_marshal_VOID__STRING_INT,
			      G_TYPE_NONE, 2, G_TYPE_STRING, G_TYPE_INT);
	signals[VIEW_DONE] =
		g_signal_new ("view_done",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_LAST,
			      G_STRUCT_OFFSET (ECalViewListenerClass, view_done),
			      NULL, NULL,
			      e_cal_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);
	signals[LISTENER_DIED] =
	  g_signal_new ("listener_died",
						G_TYPE_FROM_CLASS (klass),
						G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
						0,
						NULL, NULL,
						g_cclosure_marshal_VOID__VOID,
						G_TYPE_NONE, 0);
						
	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (klass), &dbus_glib_e_data_cal_view_listener_object_info);
}

/* Object initialization function for the live search query listener */
static void
e_cal_view_listener_init (ECalViewListener *ql)
{
}

ECalViewListener *
e_cal_view_listener_new (void)
{
	ECalViewListener *ql;

	ql = g_object_new (E_TYPE_CAL_VIEW_LISTENER, NULL);

	return ql;
}

static void
e_cal_view_listener_dispose (GObject *object)
{
	ECalViewListener *ql;
	ECalViewListenerPrivate *priv;

	g_return_if_fail (object != NULL);
	g_return_if_fail (E_IS_CAL_VIEW_LISTENER (object));

	ql = E_CAL_VIEW_LISTENER (object);
	priv = E_CAL_VIEW_LISTENER_GET_PRIVATE (ql);

/*	g_warning ("%s, %s: Emitting listener-died signal", G_STRLOC, __FUNCTION__);*/
	g_signal_emit (object, signals[LISTENER_DIED], 0);
	
	if (G_OBJECT_CLASS (parent_class)->dispose)
		(* G_OBJECT_CLASS (parent_class)->dispose) (object);
}

static ECalendarStatus
convert_status (const EDataCalCallStatus status)
{
	switch (status) {
	case Success:
		return E_CALENDAR_STATUS_OK;
	case RepositoryOffline:
		return E_CALENDAR_STATUS_REPOSITORY_OFFLINE;
	case PermissionDenied:
		return E_CALENDAR_STATUS_PERMISSION_DENIED;
	case ObjectNotFound:
		return E_CALENDAR_STATUS_OBJECT_NOT_FOUND;
	case ObjectIdAlreadyExists:
		return E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS;
	case AuthenticationFailed:
		return E_CALENDAR_STATUS_AUTHENTICATION_FAILED;
	case AuthenticationRequired:
		return E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED;
	case UnknownUser:
		return E_CALENDAR_STATUS_UNKNOWN_USER;
	case OtherError:
	default:
		return E_CALENDAR_STATUS_OTHER_ERROR;
	}
}

static GList *
build_object_list (char **seq)
{
	GList *list;
	int i;

	list = NULL;
	for (i = 0; seq[i]; i++) {
		icalcomponent *comp;
		
		comp = icalcomponent_new_from_string (seq[i]);
		if (!comp)
			continue;
		
		list = g_list_prepend (list, comp);
	}

	return list;
}

static GList *
build_uid_list (char **seq)
{
	GList *list;
	int i;

	list = NULL;
	for (i = 0; seq[i]; i++)
		list = g_list_prepend (list, g_strdup (seq[i]));

	return list;
}

static gboolean
impl_notifyObjectsAdded (ECalViewListener *ql,
			 char **objects,
			 DBusGMethodInvocation *context)
{
	GList *object_list, *l;
	
	object_list = build_object_list (objects);
	
	g_signal_emit (G_OBJECT (ql), signals[OBJECTS_ADDED], 0, object_list);

	for (l = object_list; l; l = l->next)
		icalcomponent_free (l->data);
	g_list_free (object_list);
	
	dbus_g_method_return (context);
	return TRUE;
}

static gboolean
impl_notifyObjectsModified (ECalViewListener *ql,
			    char **objects,
			    DBusGMethodInvocation *context)
{
	GList *object_list, *l;
	
	object_list = build_object_list (objects);
	
	g_signal_emit (G_OBJECT (ql), signals[OBJECTS_MODIFIED], 0, object_list);

	for (l = object_list; l; l = l->next)
		icalcomponent_free (l->data);
	g_list_free (object_list);
	
	dbus_g_method_return (context);
	return TRUE;
}

static gboolean
impl_notifyObjectsRemoved (ECalViewListener *ql,
			   char **uids,
			   DBusGMethodInvocation *context)
{
	GList *uid_list, *l;
	
	uid_list = build_uid_list (uids);
	
	g_signal_emit (G_OBJECT (ql), signals[OBJECTS_REMOVED], 0, uid_list);

	for (l = uid_list; l; l = l->next)
		g_free (l->data);
	g_list_free (uid_list);
	
	dbus_g_method_return (context);
	return TRUE;
}

static gboolean
impl_notifyQueryProgress (ECalViewListener *ql,
			  gchar *message,
			  gint16 percent,
			  DBusGMethodInvocation *context)
{
	g_signal_emit (G_OBJECT (ql), signals[VIEW_PROGRESS], 0, (const char *)message, (int)percent);
	dbus_g_method_return (context);
	return TRUE;
}

static gboolean
impl_notifyQueryDone (ECalViewListener *ql,
		      const EDataCalCallStatus status,
		      DBusGMethodInvocation *context)
{
	g_signal_emit (G_OBJECT (ql), signals[VIEW_DONE], 0, convert_status (status));
	dbus_g_method_return (context);
	return TRUE;
}

