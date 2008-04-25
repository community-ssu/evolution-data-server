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

#ifndef E_DATA_CAL_H
#define E_DATA_CAL_H

#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include "e-data-cal-common.h"
#include "e-data-cal-view.h"
#include "e-data-cal-types.h"

#define E_TYPE_DATA_CAL            (e_data_cal_get_type ())
#define E_DATA_CAL(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_TYPE_DATA_CAL, EDataCal))
#define E_DATA_CAL_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_DATA_CAL, EDataCalClass))
#define E_IS_DATA_CAL(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), E_TYPE_DATA_CAL))
#define E_IS_DATA_CAL_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_DATA_CAL))

GQuark e_data_cal_error_quark (void);
#define E_DATA_CAL_ERROR e_data_cal_error_quark ()

struct _EDataCal {
	GObject parent;
	ECalBackend *backend;
	ESource *source;
	GHashTable *live_queries;
};

struct _EDataCalClass {
	GObjectClass parent_class;
};

GType e_data_cal_get_type (void);

EDataCal *e_data_cal_new (ECalBackend *backend, ESource *source);

ECalBackend *e_data_cal_get_backend (EDataCal *cal);
ESource* e_data_cal_get_source (EDataCal *cal);

void e_data_cal_notify_read_only           (EDataCal *cal, EDataCalCallStatus status,
					    gboolean read_only);
void e_data_cal_notify_cal_address         (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					    const char *address);
void e_data_cal_notify_alarm_email_address (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					    const char *address);
void e_data_cal_notify_ldap_attribute      (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					    const char *attribute);
void e_data_cal_notify_static_capabilities (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					    const char *capabilities);

void e_data_cal_notify_open   (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status);
void e_data_cal_notify_remove (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status);

void e_data_cal_notify_object_created  (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					const char *uid, const char *object);
void e_data_cal_notify_object_modified (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, 
					const char *old_object, const char *object);
void e_data_cal_notify_object_removed  (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, 
					const char *uid, const char *old_object, const char *object);
void e_data_cal_notify_alarm_discarded (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status);

void e_data_cal_notify_objects_received (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status);
void e_data_cal_notify_objects_sent     (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GList *users,
					 const char *calobj);

void e_data_cal_notify_default_object (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				       const char *object);
void e_data_cal_notify_object         (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				       const char *object);
void e_data_cal_notify_object_list    (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				       GList *objects);

void e_data_cal_notify_query (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
			      const gchar *query_path);

void e_data_cal_notify_timezone_requested   (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					     const char *object);
void e_data_cal_notify_timezone_added       (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
					     const char *tzid);
void e_data_cal_notify_default_timezone_set (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status);

void e_data_cal_notify_changes   (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				  GList *adds, GList *modifies, GList *deletes);
void e_data_cal_notify_free_busy (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status,
				  GList *freebusy);

void e_data_cal_notify_mode  (EDataCal *cal, 
			      EDataCalViewListenerSetModeStatus status, 
			      EDataCalMode mode);

void e_data_cal_notify_auth_required (EDataCal *cal);

void e_data_cal_notify_error (EDataCal *cal, const char *message);

void e_data_cal_notify_attachment_list (EDataCal *cal, DBusGMethodInvocation *context, EDataCalCallStatus status, GSList *objects);

#endif
