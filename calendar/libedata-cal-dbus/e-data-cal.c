/* Evolution calendar client interface object
 *
 * Copyright (C) 2000 Ximian, Inc.
 * Copyright (C) 2000 Ximian, Inc.
 *
 * Authors: Federico Mena-Quintero <federico@ximian.com>
 *          Rodrigo Moya <rodrigo@ximian.com>
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

#include <libical/ical.h>
#include <glib/gi18n-lib.h>
#include <unistd.h>

/* DBus includes */
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <glib-object.h>
/**/

#include "e-data-cal.h"
#include "e-data-cal-enumtypes.h"

extern DBusGConnection *connection;

/* DBus glue */
static gboolean impl_Cal_get_uri (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_open (EDataCal *cal, gboolean only_if_exists, gchar *username, gchar *password, DBusGMethodInvocation *context);
static gboolean impl_Cal_close (EDataCal *cal, GError **error);
static gboolean impl_Cal_remove (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_isReadOnly (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_getCalAddress (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_getAlarmEmailAddress (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_getLdapAttribute (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_getStaticCapabilities (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_setMode (EDataCal *cal, EDataCalMode mode, DBusGMethodInvocation *context);
static gboolean impl_Cal_getDefaultObject (EDataCal *cal, DBusGMethodInvocation *context);
static gboolean impl_Cal_getObject (EDataCal *cal, const gchar *uid, const gchar *rid, DBusGMethodInvocation *context);
static gboolean impl_Cal_getObjectList (EDataCal *cal, const gchar *sexp, DBusGMethodInvocation *context);
static gboolean impl_Cal_getChanges (EDataCal *cal, const gchar *change_id, DBusGMethodInvocation *context);
static gboolean impl_Cal_getFreeBusy (EDataCal *cal, const char **user_list, const gulong start, const gulong end, DBusGMethodInvocation *context);
static gboolean impl_Cal_discardAlarm (EDataCal *cal, const gchar *uid, const gchar *auid, DBusGMethodInvocation *context);
static gboolean impl_Cal_createObject (EDataCal *cal, const gchar *calobj, DBusGMethodInvocation *context);
static gboolean impl_Cal_modifyObject (EDataCal *cal, const gchar *calobj, const EDataCalObjModType mod, DBusGMethodInvocation *context);
static gboolean impl_Cal_removeObject (EDataCal *cal, const gchar *uid, const gchar *rid, const EDataCalObjModType mod, DBusGMethodInvocation *context);
static gboolean impl_Cal_receiveObjects (EDataCal *cal, const gchar *calobj, DBusGMethodInvocation *context);
static gboolean impl_Cal_sendObjects (EDataCal *cal, const gchar *calobj, DBusGMethodInvocation *context);
static void impl_Cal_getAttachmentList (EDataCal *cal, gchar *uid, gchar *rid, DBusGMethodInvocation *context);
static gboolean impl_Cal_getQuery (EDataCal *cal, const gchar *sexp, const gchar *address, const gchar *listener_path, DBusGMethodInvocation *context);
static gboolean impl_Cal_getTimezone (EDataCal *cal, const gchar *tzid, DBusGMethodInvocation *context);
static gboolean impl_Cal_addTimezone (EDataCal *cal, const gchar *tz, DBusGMethodInvocation *context);
static gboolean impl_Cal_setDefaultTimezone (EDataCal *cal, const gchar *tzid, DBusGMethodInvocation *context);
#include "e-data-cal-glue.h"

enum
{
  AUTH_REQUIRED,
  BACKEND_ERROR,
  READ_ONLY,
  MODE,
  LAST_SIGNAL
};

static guint signals[LAST_SIGNAL] = { 0 };

/* Generate the GObject boilerplate */
G_DEFINE_TYPE(EDataCal, e_data_cal, G_TYPE_OBJECT)

/* Create the EDataCal error quark */
GQuark
e_data_cal_error_quark (void)
{
  static GQuark quark = 0;
  if (!quark)
    quark = g_quark_from_static_string ("e_data_cal_error");
  return quark;
}

/* Class init */
static void
e_data_cal_class_init (EDataCalClass *e_data_cal_class)
{
	signals[AUTH_REQUIRED] =
	  g_signal_new ("auth-required",
						G_OBJECT_CLASS_TYPE (e_data_cal_class),
						G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
						0,
						NULL, NULL,
						g_cclosure_marshal_VOID__VOID,
						G_TYPE_NONE, 0);
	signals[BACKEND_ERROR] =
	  g_signal_new ("backend-error",
						G_OBJECT_CLASS_TYPE (e_data_cal_class),
						G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
						0,
						NULL, NULL,
						g_cclosure_marshal_VOID__STRING,
						G_TYPE_NONE, 1, G_TYPE_STRING);
	signals[READ_ONLY] =
	  g_signal_new ("readonly",
						G_OBJECT_CLASS_TYPE (e_data_cal_class),
						G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
						0,
						NULL, NULL,
						g_cclosure_marshal_VOID__BOOLEAN,
						G_TYPE_NONE, 1, G_TYPE_BOOLEAN);
	signals[MODE] =
	  g_signal_new ("mode",
						G_OBJECT_CLASS_TYPE (e_data_cal_class),
						G_SIGNAL_RUN_LAST | G_SIGNAL_DETAILED,
						0,
						NULL, NULL,
						g_cclosure_marshal_VOID__INT,
						G_TYPE_NONE, 1, G_TYPE_INT);
	
	dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (e_data_cal_class), &dbus_glib_e_data_cal_object_info);
	
	dbus_g_error_domain_register (E_DATA_CAL_ERROR, NULL, E_TYPE_DATA_CAL_CALL_STATUS);	
}

/* Instance init */
static void
e_data_cal_init (EDataCal *ecal)
{
}

EDataCal *
e_data_cal_new (ECalBackend *backend, ESource *source)
{
	EDataCal *cal;
	cal = g_object_new (E_TYPE_DATA_CAL, NULL);
	cal->backend = backend;
	cal->source = source;
	cal->live_queries = g_hash_table_new_full (g_str_hash, g_str_equal,
						    (GDestroyNotify) g_free,
						    (GDestroyNotify) g_object_unref);
	return cal;
}

ESource*
e_data_cal_get_source (EDataCal *cal)
{
  return cal->source;
}

ECalBackend*
e_data_cal_get_backend (EDataCal *cal)
{
  return cal->backend;
}

/* EDataCal::getUri method */
static gboolean
impl_Cal_get_uri (EDataCal *cal, DBusGMethodInvocation *context)
{
	dbus_g_method_return (context, g_strdup (e_cal_backend_get_uri (cal->backend)));
	return TRUE;
}

/* EDataCal::open method */
static gboolean
impl_Cal_open (EDataCal *cal,
	       gboolean only_if_exists,
	       gchar *username,
	       gchar *password, DBusGMethodInvocation *context)
{
	e_cal_backend_open (cal->backend, cal, context, only_if_exists, username, password);
	return TRUE;
}

void 
e_data_cal_notify_open (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot open calendar")));
	else
		dbus_g_method_return (context);
}

/* EDataCal::close method */
static gboolean
impl_Cal_close (EDataCal *cal, GError **error)
{
	g_object_unref (cal);
	return TRUE;
}

/* EDataCal::remove method */
static gboolean
impl_Cal_remove (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_remove (cal->backend, cal, context);
	return TRUE;
}

void
e_data_cal_notify_remove (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot remove calendar")));
	else
		dbus_g_method_return (context);
}

/* EDataCal::isReadOnly method */
static gboolean
impl_Cal_isReadOnly (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_is_read_only (cal->backend, cal);
	dbus_g_method_return (context);
	return TRUE;
}

/* EDataCal::getCalAddress method */
static gboolean
impl_Cal_getCalAddress (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_get_cal_address (cal->backend, cal, context);
	return TRUE;
}

void 
e_data_cal_notify_cal_address (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *address)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar address")));
	else
		dbus_g_method_return (context, address ? address : "");
}

/* EDataCal::getAlarmEmailAddress method */
static gboolean
impl_Cal_getAlarmEmailAddress (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_get_alarm_email_address (cal->backend, cal, context);
	return TRUE;
}
		       
void
e_data_cal_notify_alarm_email_address (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *address)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar alarm e-mail address")));
	else
		dbus_g_method_return (context, address ? address : "");
}

/* EDataCal::getLdapAttribute method */
static gboolean
impl_Cal_getLdapAttribute (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_get_ldap_attribute (cal->backend, cal, context);
	return TRUE;
}

void
e_data_cal_notify_ldap_attribute (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *attribute)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar's ldap attribute")));
	else
		dbus_g_method_return (context, attribute ? attribute : "");
}

/* EDataCal::getSchedulingInformation method */
static gboolean
impl_Cal_getStaticCapabilities (EDataCal *cal, DBusGMethodInvocation *context)
{
	e_cal_backend_get_static_capabilities (cal->backend, cal, context);
	return TRUE;
}

void
e_data_cal_notify_static_capabilities (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *capabilities)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar scheduling information")));
	else
		dbus_g_method_return (context, capabilities ? capabilities : "");
}

/* EDataCal::setMode method */
static gboolean
impl_Cal_setMode (EDataCal *cal,
		  EDataCalMode mode,
		  DBusGMethodInvocation *context)
{
	e_cal_backend_set_mode (cal->backend, mode);
	dbus_g_method_return (context);
	return TRUE;
}

/* EDataCal::getDefaultObject method */
static gboolean
impl_Cal_getDefaultObject (EDataCal *cal, DBusGMethodInvocation *context)
{
 	e_cal_backend_get_default_object (cal->backend, cal, context);
	return TRUE;
}

void
e_data_cal_notify_default_object (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *object)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve default calendar object path")));
	else
		dbus_g_method_return (context, object ? object : "");
}

/* EDataCal::getObject method */
static gboolean
impl_Cal_getObject (EDataCal *cal,
		    const gchar *uid,
		    const gchar *rid,
		    DBusGMethodInvocation *context)
{
	e_cal_backend_get_object (cal->backend, cal, context, uid, rid);
	return TRUE;
}

void
e_data_cal_notify_object (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *object)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar object path")));
	else
		dbus_g_method_return (context, object ? object : "");
}

/* EDataCal::getObjectList method */
static gboolean
impl_Cal_getObjectList (EDataCal *cal,
			const gchar *sexp,
			DBusGMethodInvocation *context)
{
	EDataCalView *query = g_hash_table_lookup (cal->live_queries, sexp);
	if (query) {
		GList *matched_objects;

		matched_objects = e_data_cal_view_get_matched_objects (query);
		e_data_cal_notify_object_list (
			cal, context,
			e_data_cal_view_is_done (query) ? e_data_cal_view_get_done_status (query) : Success,
			matched_objects);

		g_list_free (matched_objects);
	} else
		e_cal_backend_get_object_list (cal->backend, cal, context, sexp);
	
	return TRUE;
}

void
e_data_cal_notify_object_list (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GList *objects)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar object list")));
	} else {
		char **seq = NULL;
		GList *l;
		int i;
	
		seq = g_new0 (char *, g_list_length (objects)+1);
		for (l = objects, i = 0; l; l = l->next, i++) {
			seq[i] = l->data;
		}
		
		dbus_g_method_return (context, seq);
	
		g_free (seq);
	}
}

/* EDataCal::getChanges method */
static gboolean
impl_Cal_getChanges (EDataCal *cal,
		     const gchar *change_id,
		     DBusGMethodInvocation *context)
{
       e_cal_backend_get_changes (cal->backend, cal, context, change_id);
		 return TRUE;
}

void
e_data_cal_notify_changes (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, 
			   GList *adds, GList *modifies, GList *deletes)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar changes")));
	} else {
		char **additions, **modifications, **removals;
		GList *l;	
		int i;
	
		additions = NULL;
		if (adds) {
			additions = g_new0 (char *, g_list_length (adds) + 1);
			if (additions)
				for (i = 0, l = adds; l; i++, l = l->next)
					additions[i] = g_strdup (l->data);
		}
	
		modifications = NULL;
		if (modifies) {
			modifications = g_new0 (char *, g_list_length (modifies) + 1);
			if (modifications)
				for (i = 0, l = modifies; l; i++, l = l->next)
					modifications[i] = g_strdup (l->data);
		}
	
		removals = NULL;
		if (deletes) {
			removals = g_new0 (char *, g_list_length (deletes) + 1);
			if (removals)
				for (i = 0, l = deletes; l; i++, l = l->next)
					removals[i] = g_strdup (l->data);
		}
	
		dbus_g_method_return (context, additions, modifications, removals);
		if (additions) g_strfreev (additions);
		if (modifications) g_strfreev (modifications);
		if (removals) g_strfreev (removals);
	}
}

/* EDataCal::getFreeBusy method */
static gboolean
impl_Cal_getFreeBusy (EDataCal *cal,
		      const char **user_list,
		      const gulong start,
		      const gulong end,
		      DBusGMethodInvocation *context)
{
	GList *users = NULL;

	if (user_list) {
		int i;

		for (i = 0; user_list[i]; i++)
			users = g_list_append (users, (gpointer)user_list[i]);
	}

	/* call the backend's get_free_busy method */
	e_cal_backend_get_free_busy (cal->backend, cal, context, users, (time_t)start, (time_t)end);
	
	return TRUE;
}

void
e_data_cal_notify_free_busy (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GList *freebusy)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot retrieve calendar free/busy list")));
	} else {
		char **seq;
		GList *l;
		int i;
	
		seq = NULL;
		if (freebusy) {
			seq = g_new0 (char *, g_list_length (freebusy));
			if (seq)
				for (i = 0, l = freebusy; l; i++, l = l->next)
					seq[i] = g_strdup (l->data);
		}
		
		dbus_g_method_return (context, seq);
		g_strfreev (seq);
	}
}

/* EDataCal::discardAlarm method */
static gboolean
impl_Cal_discardAlarm (EDataCal *cal,
		       const gchar *uid,
		       const gchar *auid,
		       DBusGMethodInvocation *context)
{
	e_cal_backend_discard_alarm (cal->backend, cal, context, uid, auid);
	return TRUE;
}

void
e_data_cal_notify_alarm_discarded (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot discard calendar alarm")));
	else
		dbus_g_method_return (context);
}

/* EDataCal::createObject method */
static gboolean
impl_Cal_createObject (EDataCal *cal,
		       const gchar *calobj,
		       DBusGMethodInvocation *context)
{
	e_cal_backend_create_object (cal->backend, cal, context, calobj);
	return TRUE;
}

void
e_data_cal_notify_object_created (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				  const char *uid, const char *object)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot create calendar object")));
	} else {
		e_cal_backend_notify_object_created (cal->backend, object);
		dbus_g_method_return (context, uid ? uid : "");
	}
}

/* EDataCal::modifyObject method */
static gboolean
impl_Cal_modifyObject (EDataCal *cal,
		       const gchar *calobj,
		       const EDataCalObjModType mod,
		       DBusGMethodInvocation *context)
{
	e_cal_backend_modify_object (cal->backend, cal, context, calobj, mod);
	return TRUE;
}

void
e_data_cal_notify_object_modified (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, 
				   const char *old_object, const char *object)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot modify calender object")));
	} else {
		e_cal_backend_notify_object_modified (cal->backend, old_object, object);
		dbus_g_method_return (context);
	}
}

/* EDataCal::removeObject method */
static gboolean
impl_Cal_removeObject (EDataCal *cal,
		       const gchar *uid,
		       const gchar *rid,
		       const EDataCalObjModType mod,
		       DBusGMethodInvocation *context)
{
	e_cal_backend_remove_object (cal->backend, cal, context, uid, rid, mod);
	return TRUE;
}

void
e_data_cal_notify_object_removed (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, 
				  const char *uid, const char *old_object, const char *object)
{

	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot remove calendar object")));
	} else {
		e_cal_backend_notify_object_removed (cal->backend, uid, old_object, object);
		dbus_g_method_return (context);
	}
}

/* EDataCal::receiveObjects method */
static gboolean
impl_Cal_receiveObjects (EDataCal *cal,
			 const gchar *calobj,
			 DBusGMethodInvocation *context)
{
	e_cal_backend_receive_objects (cal->backend, cal, context, calobj);
	return TRUE;
}

void
e_data_cal_notify_objects_received (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot receive calendar objects")));
	else
		dbus_g_method_return (context);
}

/* EDataCal::sendObjects method */
static gboolean
impl_Cal_sendObjects (EDataCal *cal,
		      const gchar *calobj,
		      DBusGMethodInvocation *context)
{
	e_cal_backend_send_objects (cal->backend, cal, context, calobj);
	return TRUE;
}

void
e_data_cal_notify_objects_sent (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GList *users, const char *calobj)
{
	if (status != Success) {
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Cannot send calendar objects")));
	} else {
		char **users_array = NULL;
	
		if (users) {
			GList *l;
			char **user;
			
			users_array = g_new0 (char *, g_list_length (users)+1);
			if (users_array)
				for (l = users, user = users_array; l != NULL; l = l->next, user++)
					*user = g_strdup (l->data);
		}
		else
			users_array = g_new0 (char *, 1);
	
		dbus_g_method_return (context, users_array, calobj ? calobj : "");
		if (users_array)
			g_strfreev (users_array);
	}
}

/* EDataCal::getAttachmentList method */
static void
impl_Cal_getAttachmentList (EDataCal *cal,
                   gchar *uid,
                   gchar *rid,
                   DBusGMethodInvocation *context)
{
	e_cal_backend_get_attachment_list (cal->backend, cal, context, uid, rid);
}

/**
 * e_data_cal_notify_attachment_list:
 * @cal: A calendar client interface.
 * @status: Status code.
 * @attachments: List of retrieved attachment uri's.
 *
 * Notifies listeners of the completion of the get_attachment_list method call.+ */
void
e_data_cal_notify_attachment_list (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GSList *attachments)
{
	char **seq;
	GSList *l;
	int i;
	
	seq = g_new0 (char *, g_slist_length (attachments));
	for (l = attachments, i = 0; l; l = l->next, i++) {
		seq[i] = g_strdup (l->data);
	}

	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Could not retrieve attachment list")));
	else
		dbus_g_method_return (context, seq);
}

/* Function to get a new EDataCalView path, used by getQuery below */
static gchar*
construct_calview_path (void)
{
  static guint counter = 1;
  return g_strdup_printf ("/org/gnome/evolution/dataserver/calendar/CalView/%d/%d", getpid(), counter++);
}

/* EDataCal::getQuery method */
static gboolean
impl_Cal_getQuery (EDataCal *cal,
		   const gchar *sexp,
		   const gchar *address, const gchar *listener_path,
		   DBusGMethodInvocation *context)
{
	EDataCalView *query;
	ECalBackendSExp *obj_sexp;
	DBusGProxy *ql;
	gchar *path;
	
	ql = dbus_g_proxy_new_for_name (connection, address, listener_path, "org.gnome.evolution.dataserver.calendar.DataCalViewListener");
	g_assert (ql != NULL);
	
	/* first see if we already have the query in the cache */
	query = g_hash_table_lookup (cal->live_queries, sexp);
	if (query) {
		e_data_cal_view_add_listener (query, ql);
		e_data_cal_notify_query (cal, context, Success, query->path);
		return TRUE;
	}

	/* we handle this entirely here, since it doesn't require any
	   backend involvement now that we have e_cal_view_start to
	   actually kick off the search. */

	obj_sexp = e_cal_backend_sexp_new (sexp);
	if (!obj_sexp) {
		e_data_cal_notify_query (cal, context, InvalidQuery, NULL);

		return TRUE;
	}

	path = construct_calview_path ();
	query = e_data_cal_view_new (cal->backend, ql, path, obj_sexp);
	if (!query) {
		g_object_unref (obj_sexp);
		e_data_cal_notify_query (cal, context, OtherError, NULL);

		return TRUE;
	}
	dbus_g_connection_register_g_object (connection, path, G_OBJECT (query));

	g_hash_table_insert (cal->live_queries, g_strdup (sexp), query);
	e_cal_backend_add_query (cal->backend, query);

	e_data_cal_notify_query (cal, context, Success, path);
	
	return TRUE;
}

/*
 * Only have a seperate notify function to follow suit with the rest of this
 * file - it'd be much easier to just do the return in the above function
 */
void
e_data_cal_notify_query (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const gchar *query_path)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Could not complete calendar query")));
	else
		dbus_g_method_return (context, query_path);
}

/* EDataCal::getTimezone method */
static gboolean
impl_Cal_getTimezone (EDataCal *cal,
		      const gchar *tzid,
		      DBusGMethodInvocation *context)
{
	e_cal_backend_get_timezone (cal->backend, cal, context, tzid);
	return TRUE;
}

void
e_data_cal_notify_timezone_requested (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *object)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Could not retrieve calendar time zone")));
	else
		dbus_g_method_return (context, object ? object : "");
}

/* EDataCal::addTimezone method */
static gboolean
impl_Cal_addTimezone (EDataCal *cal,
		      const gchar *tz,
		      DBusGMethodInvocation *context)
{
	e_cal_backend_add_timezone (cal->backend, cal, context, tz);
	return TRUE;
}

void
e_data_cal_notify_timezone_added (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, const char *tzid)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Could not add calendar time zone")));
	else
		dbus_g_method_return (context, tzid ? tzid : "");
}

/* EDataCal::setDefaultTimezone method */
static gboolean
impl_Cal_setDefaultTimezone (EDataCal *cal,
			     const gchar *tzid,
			     DBusGMethodInvocation *context)
{
	e_cal_backend_set_default_timezone (cal->backend, cal, context, tzid);
	return TRUE;
}

void
e_data_cal_notify_default_timezone_set (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status)
{
	if (status != Success)
		dbus_g_method_return_error (context, g_error_new (E_DATA_CAL_ERROR, status, _("Could not set default calendar time zone")));
	else
		dbus_g_method_return (context);
}

/* Signal emitting callbacks */
void 
e_data_cal_notify_auth_required (EDataCal *cal)
{
	g_signal_emit (cal, signals[AUTH_REQUIRED], 0);
}

void
e_data_cal_notify_error (EDataCal *cal, const char *message)
{
	g_signal_emit (cal, signals[BACKEND_ERROR], 0, message);
}

void
e_data_cal_notify_mode (EDataCal *cal,
			EDataCalViewListenerSetModeStatus status,
			EDataCalMode mode)
{
	g_signal_emit (cal, signals[MODE], 0, mode);
}

void 
e_data_cal_notify_read_only (EDataCal *cal, EDataCalCallStatus status, gboolean read_only)
{
	g_signal_emit (cal, signals[READ_ONLY], 0, read_only);
}		       
/*
TODO: finalise dispose
*/
