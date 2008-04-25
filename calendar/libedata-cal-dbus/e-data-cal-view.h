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

#ifndef E_DATA_CAL_VIEW_H
#define E_DATA_CAL_VIEW_H

#include <glib.h>
#include <glib-object.h>
#include <dbus/dbus-glib.h>
#include "e-data-cal-common.h"
#include "e-cal-backend-sexp.h"
#include "e-data-cal-types.h"

#define E_DATA_CAL_VIEW_TYPE            (e_data_cal_view_get_type ())
#define QUERY(obj)            (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_DATA_CAL_VIEW_TYPE, EDataCalView))
#define E_DATA_CAL_VIEW_CLASS(klass)    (G_TYPE_CHECK_CLASS_CAST ((klass), E_DATA_CAL_VIEW_TYPE, EDataCalViewClass))
#define IS_QUERY(obj)         (G_TYPE_CHECK_INSTANCE_TYPE ((obj), E_DATA_CAL_VIEW_TYPE))
#define IS_E_DATA_CAL_VIEW_CLASS(klass) (G_TYPE_CHECK_CLASS_TYPE ((klass), E_DATA_CAL_VIEW_TYPE))

typedef struct _EDataCalViewPrivate EDataCalViewPrivate;

struct _EDataCalView {
	GObject parent;
	
	/* DBus path, for returning in-progress queries */
	const gchar *path;
};

struct _EDataCalViewClass {
	GObjectClass parent_class;
};

GType                 e_data_cal_view_get_type (void);
EDataCalView         *e_data_cal_view_new (ECalBackend *backend, DBusGProxy *ql,
							char *path, ECalBackendSExp *sexp);
void                  e_data_cal_view_add_listener (EDataCalView *query, DBusGProxy *ql);
const char           *e_data_cal_view_get_text (EDataCalView *query);
ECalBackendSExp      *e_data_cal_view_get_object_sexp (EDataCalView *query);
gboolean              e_data_cal_view_object_matches (EDataCalView *query, const char *object);

GList                *e_data_cal_view_get_matched_objects (EDataCalView *query);
gboolean              e_data_cal_view_is_started (EDataCalView *query);
gboolean              e_data_cal_view_is_done (EDataCalView *query);
EDataCalCallStatus e_data_cal_view_get_done_status (EDataCalView *query);

void                  e_data_cal_view_notify_objects_added (EDataCalView       *query,
							    const GList *objects);
void                  e_data_cal_view_notify_objects_added_1 (EDataCalView       *query,
							      const char *object);
void                  e_data_cal_view_notify_objects_modified (EDataCalView       *query,
							       const GList *objects);
void                  e_data_cal_view_notify_objects_modified_1 (EDataCalView       *query,
								 const char *object);
void                  e_data_cal_view_notify_objects_removed (EDataCalView       *query,
							      const GList *uids);
void                  e_data_cal_view_notify_objects_removed_1 (EDataCalView       *query,
								const char *uid);
void                  e_data_cal_view_notify_progress (EDataCalView      *query,
						       const char *message,
						       int         percent);
void                  e_data_cal_view_notify_done (EDataCalView                               *query,
						   EDataCalCallStatus status);

#endif
