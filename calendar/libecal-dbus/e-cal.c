/*-*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* Evolution calendar ecal
 *
 * Copyright (C) 2001 Ximian, Inc.
 * Copyright (C) 2004 Novell, Inc.
 *
 * Authors: Federico Mena-Quintero <federico@ximian.com>
 *          Rodrigo Moya <rodrigo@novell.com>
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

#include <unistd.h>
#include <pthread.h>
#include <string.h>
#include <glib/gi18n-lib.h>

#include <libedataserver/e-url.h>

#define DBUS_API_SUBJECT_TO_CHANGE
#include <dbus/dbus-glib-lowlevel.h>
#include <dbus/dbus.h>
#include <dbus/dbus-glib.h>
#include <glib-object.h>

#include "e-cal-marshal.h"
#include "e-cal-time-util.h"
#include "e-cal-view-listener.h"
#include "e-cal.h"
#include "e-data-cal-factory-bindings.h"
#include "e-data-cal-bindings.h"
#include <libedata-cal-dbus/e-data-cal-types.h>

static DBusGConnection *connection = NULL;
static DBusGProxy *factory_proxy = NULL;

G_DEFINE_TYPE(ECal, e_cal, G_TYPE_OBJECT)
#define E_CAL_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_CAL, ECalPrivate))

static gboolean open_calendar (ECal *ecal, gboolean only_if_exists, GError **error, ECalendarStatus *status, gboolean needs_auth, gboolean async);
static void e_cal_dispose (GObject *object);
static void e_cal_finalize (GObject *object);

/* Private part of the ECal structure */
struct _ECalPrivate {
	/* Load state to avoid multiple loads */
	ECalLoadState load_state;

	/* URI of the calendar that is being loaded or is already loaded, or
	 * NULL if we are not loaded.
	 */
	ESource *source;
	char *uri;
	ECalSourceType type;
	
	/* Email address associated with this calendar, or NULL */
	char *cal_address;
	char *alarm_email_address;
	char *ldap_attribute;

	/* Scheduling info */
	char *capabilities;
	
	int mode;
	
	gboolean read_only;
	
	DBusGProxy *proxy;

	/* The authentication function */
	ECalAuthFunc auth_func;
	gpointer auth_user_data;

	/* A cache of timezones retrieved from the server, to avoid getting
	   them repeatedly for each get_object() call. */
	GHashTable *timezones;

	/* The default timezone to use to resolve DATE and floating DATE-TIME
	   values. */
	icaltimezone *default_zone;

	char *local_attachment_store;
};



/* Signal IDs */
enum {
	CAL_OPENED,
	CAL_SET_MODE,
	BACKEND_ERROR,
	BACKEND_DIED,
	LAST_SIGNAL
};

static guint e_cal_signals[LAST_SIGNAL];

static GObjectClass *parent_class;

#ifdef __PRETTY_FUNCTION__
#define e_return_error_if_fail(expr,error_code)	G_STMT_START{		\
     if G_LIKELY(expr) { } else						\
       {								\
	 g_log (G_LOG_DOMAIN,						\
		G_LOG_LEVEL_CRITICAL,					\
		"file %s: line %d (%s): assertion `%s' failed",		\
		__FILE__,						\
		__LINE__,						\
		__PRETTY_FUNCTION__,					\
		#expr);							\
	 g_set_error (error, E_CALENDAR_ERROR, (error_code),                \
		"file %s: line %d (%s): assertion `%s' failed",		\
		__FILE__,						\
		__LINE__,						\
		__PRETTY_FUNCTION__,					\
		#expr);							\
	 return FALSE;							\
       };				}G_STMT_END
#else
#define e_return_error_if_fail(expr,error_code)	G_STMT_START{		\
     if G_LIKELY(expr) { } else						\
       {								\
	 g_log (G_LOG_DOMAIN,						\
		G_LOG_LEVEL_CRITICAL,					\
		"file %s: line %d: assertion `%s' failed",		\
		__FILE__,						\
		__LINE__,						\
		#expr);							\
	 g_set_error (error, E_CALENDAR_ERROR, (error_code),                \
		"file %s: line %d: assertion `%s' failed",		\
		__FILE__,						\
		__LINE__,						\
		#expr);							\
	 return FALSE;							\
       };				}G_STMT_END
#endif

/**
 * If the GError is a remote error, extract the EBookStatus embedded inside.
 * Otherwise return CORBA_EXCEPTION (I know this is DBus...).
 */
 /* TODO: Do this better :p */
static ECalendarStatus
get_status_from_error (GError *error)
{
  if G_LIKELY (error == NULL)
    return Success;
  if (error->domain == DBUS_GERROR && error->code == DBUS_GERROR_REMOTE_EXCEPTION) {
    const char *name;
    name = dbus_g_error_get_name (error);
	 g_warning ("Encountered error: %s", name);
    if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.repositoryoffline") == 0) {
      return E_CALENDAR_STATUS_REPOSITORY_OFFLINE;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.permissiondenied") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.invalidrange") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.objectnotfound") == 0) {
      return E_CALENDAR_STATUS_OBJECT_NOT_FOUND;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.invalidobject") == 0) {
      return E_CALENDAR_STATUS_INVALID_OBJECT;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.objectidalreadyexists") == 0) {
      return E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.authenticationfailed") == 0) {
      return E_CALENDAR_STATUS_AUTHENTICATION_FAILED;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.authenticationrequired") == 0) {
      return E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED;
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.unsupportedfield") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.unsupportedmethod") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.unsupportedauthenticationmethod") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.tlsnotavailable") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.nosuchcal") == 0) {
      return E_CALENDAR_STATUS_NO_SUCH_CALENDAR;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.unknownuser") == 0) {
      return E_CALENDAR_STATUS_UNKNOWN_USER;
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.offlineunavailable") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.searchsizelimitexceeded") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.searchtimelimitexceeded") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.invalidquery") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
/*    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.queryrefused") == 0) {
      return E_CALENDAR_STATUS_PERMISSION_DENIED;*/
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.couldnotcancel") == 0) {
      return E_CALENDAR_STATUS_COULD_NOT_CANCEL;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.othererror") == 0) {
      return E_CALENDAR_STATUS_OTHER_ERROR;
    } else if (strcmp (name, "org.gnome.evolution.dataserver.calendar.Cal.invalidserverversion") == 0) {
      return E_CALENDAR_STATUS_INVALID_SERVER_VERSION;
    } else {
      g_warning ("Unmatched error name %s", name);
      return E_CALENDAR_STATUS_OTHER_ERROR;
    }
  } else {
    /* In this case the error was caused by DBus */
    return E_CALENDAR_STATUS_CORBA_EXCEPTION;
  }
}

/**
 * If the specified GError is a remote error, then create a new error
 * representing the remote error.  If the error is anything else, then leave it
 * alone.
 */
static gboolean
unwrap_gerror(GError **error)
{
	if (*error == NULL)
		return TRUE;
	if ((*error)->domain == DBUS_GERROR && (*error)->code == DBUS_GERROR_REMOTE_EXCEPTION) {
		GError *new_error = NULL;
		gint code;
		code = get_status_from_error (*error);
		new_error = g_error_new_literal (E_CALENDAR_ERROR, code, (*error)->message);
		g_error_free (*error);
		*error = new_error;
	}
	return FALSE;
}

#define E_CALENDAR_CHECK_STATUS(status,error) G_STMT_START{ \
	if ((status) == E_CALENDAR_STATUS_OK) { \
		return TRUE; \
	} \
	else { \
		if (error) { \
			g_warning ("%s, %s: %s", G_STRLOC, __FUNCTION__, (*error)->message); \
			return unwrap_gerror (error); \
		} \
		const char *msg; \
		msg = e_cal_get_error_message ((status)); \
		g_set_error ((error), E_CALENDAR_ERROR, (status), msg, (status)); \
		return FALSE; \
	}}G_STMT_END
	
/* Error quark */
GQuark
e_calendar_error_quark (void)
{
	static GQuark q = 0;
	if (q == 0)
		q = g_quark_from_static_string ("e-calendar-error-quark");

	return q;
}

/* Class initialization function for the calendar ecal */
static void
e_cal_class_init (ECalClass *klass)
{
	GObjectClass *object_class;

	object_class = (GObjectClass *) klass;

	parent_class = g_type_class_peek_parent (klass);

	e_cal_signals[CAL_OPENED] =
		g_signal_new ("cal_opened",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (ECalClass, cal_opened),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__INT,
			      G_TYPE_NONE, 1, G_TYPE_INT);
	e_cal_signals[CAL_SET_MODE] =
		g_signal_new ("cal_set_mode",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (ECalClass, cal_set_mode),
			      NULL, NULL,
			      e_cal_marshal_VOID__ENUM_ENUM,
			      G_TYPE_NONE, 2,
			      E_CAL_SET_MODE_STATUS_ENUM_TYPE,
			      CAL_MODE_ENUM_TYPE);
	e_cal_signals[BACKEND_ERROR] =
		g_signal_new ("backend_error",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (ECalClass, backend_error),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__STRING,
			      G_TYPE_NONE, 1,
			      G_TYPE_STRING);
	e_cal_signals[BACKEND_DIED] =
		g_signal_new ("backend_died",
			      G_TYPE_FROM_CLASS (klass),
			      G_SIGNAL_RUN_FIRST,
			      G_STRUCT_OFFSET (ECalClass, backend_died),
			      NULL, NULL,
			      g_cclosure_marshal_VOID__VOID,
			      G_TYPE_NONE, 0);

	klass->cal_opened = NULL;
	klass->backend_died = NULL;

	object_class->dispose = e_cal_dispose;
	object_class->finalize = e_cal_finalize;
	
	g_type_class_add_private (klass, sizeof (ECalPrivate));
}

/* Object initialization function for the calendar ecal */
static void
e_cal_init (ECal *ecal)
{
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);

	priv->load_state = E_CAL_LOAD_NOT_LOADED;
	priv->uri = NULL;
	priv->local_attachment_store = NULL;

	priv->cal_address = NULL;
	priv->alarm_email_address = NULL;
	priv->ldap_attribute = NULL;
	priv->capabilities = FALSE;
	priv->proxy = NULL;
	priv->timezones = g_hash_table_new (g_str_hash, g_str_equal);
	priv->default_zone = icaltimezone_get_utc_timezone ();
}

/* Dispose handler for the calendar ecal */
static void
e_cal_dispose (GObject *object)
{
	ECal *ecal;
	ECalPrivate *priv;
	GError *error = NULL;
	
	ecal = E_CAL (object);
	priv = E_CAL_GET_PRIVATE (ecal);
	
	if (priv->proxy)
		org_gnome_evolution_dataserver_calendar_Cal_close (priv->proxy, &error);
		
	if (G_OBJECT_CLASS (parent_class)->dispose)
		(* G_OBJECT_CLASS (parent_class)->dispose) (object);
}

/* Finalize handler for the calendar ecal */
static void
e_cal_finalize (GObject *object)
{
	g_warning("TODO: free memory");
}

/* one-time start up for libecal */
static gboolean
e_cal_activate(GError **error)
{
	static gboolean done = FALSE;
	DBusError derror;
	
	if (G_LIKELY(done))
		return TRUE;

	if (!connection) {
		//if (!g_thread_supported ()) g_thread_init (NULL);
		//dbus_g_thread_init ();
		connection = dbus_g_bus_get (DBUS_BUS_SESSION, error);
		if (!connection)
			return FALSE;
	}
	
	dbus_error_init (&derror);
	if (!dbus_bus_start_service_by_name (dbus_g_connection_get_connection (connection),
													"org.gnome.evolution.dataserver.Calendar",
													0, NULL, &derror))
	{
		dbus_set_g_error (error, &derror);
		dbus_error_free (&derror);
		return FALSE;
	}
	
	if (!factory_proxy) {
		factory_proxy = dbus_g_proxy_new_for_name_owner (connection,
							"org.gnome.evolution.dataserver.Calendar",
							"/org/gnome/evolution/dataserver/calendar/CalFactory",
							"org.gnome.evolution.dataserver.calendar.CalFactory",
							error);
		if (!factory_proxy)
			return FALSE;
	}

	done = TRUE;
	return TRUE;
}

/**
 * e_cal_set_mode_status_enum_get_type:
 *
 * Registers the #ECalSetModeStatusEnum type with glib.
 *
 * Return value: the ID of the #ECalSetModeStatusEnum type.
 */
GType
e_cal_set_mode_status_enum_get_type (void)
{
	static GType e_cal_set_mode_status_enum_type = 0;
	
	if (!e_cal_set_mode_status_enum_type) {
		static GEnumValue values [] = {
			{ E_CAL_SET_MODE_SUCCESS,          "ECalSetModeSuccess",         "success"     },
			{ E_CAL_SET_MODE_ERROR,            "ECalSetModeError",           "error"       },
			{ E_CAL_SET_MODE_NOT_SUPPORTED,    "ECalSetModeNotSupported",    "unsupported" },
			{ -1,                                   NULL,                              NULL          }
		};

		e_cal_set_mode_status_enum_type =
			g_enum_register_static ("ECalSetModeStatusEnum", values);
	}

	return e_cal_set_mode_status_enum_type;
}

/**
 * cal_mode_enum_get_type:
 *
 * Registers the #CalModeEnum type with glib.
 *
 * Return value: the ID of the #CalModeEnum type.
 */
GType
cal_mode_enum_get_type (void)
{
	static GType cal_mode_enum_type = 0;

	if (!cal_mode_enum_type) {
		static GEnumValue values [] = {
			{ CAL_MODE_INVALID,                     "CalModeInvalid",                  "invalid" },
			{ CAL_MODE_LOCAL,                       "CalModeLocal",                    "local"   },
			{ CAL_MODE_REMOTE,                      "CalModeRemote",                   "remote"  },
			{ CAL_MODE_ANY,                         "CalModeAny",                      "any"     },
			{ -1,                                   NULL,                              NULL      }
		};

		cal_mode_enum_type = g_enum_register_static ("CalModeEnum", values);
	}

	return cal_mode_enum_type;
}

static EDataCalObjType
convert_type (ECalSourceType type) 
{
	switch (type){
	case E_CAL_SOURCE_TYPE_EVENT:
		return Event;
	case E_CAL_SOURCE_TYPE_TODO:
		return Todo;
	case E_CAL_SOURCE_TYPE_JOURNAL:
		return Journal;
	default:
		return AnyType;
	}
	
	return AnyType;
}


static gboolean  
reopen_with_auth (gpointer data)
{
	ECalendarStatus status;
	
	open_calendar (E_CAL (data), TRUE, NULL, &status, TRUE, FALSE);
	return FALSE;
}

static void
auth_required_cb (DBusGProxy *proxy, ECal *cal)
{
	g_idle_add (reopen_with_auth, (gpointer)cal);
}

static void
mode_cb (DBusGProxy *proxy, EDataCalMode mode, ECal *cal)
{
	g_signal_emit (G_OBJECT (cal), e_cal_signals[CAL_SET_MODE],
		       0, E_CALENDAR_STATUS_OK, mode);
}

static void
readonly_cb (DBusGProxy *proxy, gboolean read_only, ECal *cal)
{
	ECalPrivate *priv;
	g_return_if_fail(cal && E_IS_CAL (cal));
	
	priv = E_CAL_GET_PRIVATE (cal);
	priv->read_only = read_only;
}

/*
static void
backend_died_cb (EComponentListener *cl, gpointer user_data)
{
	ECalPrivate *priv;
	ECal *ecal = (ECal *) user_data;

	priv = E_CAL_GET_PRIVATE (ecal);
	priv->load_state = E_CAL_LOAD_NOT_LOADED;
	g_signal_emit (G_OBJECT (ecal), e_cal_signals[BACKEND_DIED], 0);
}*/

typedef struct
{
	ECal *ecal;
	char *message;
}  ECalErrorData;

static gboolean
backend_error_idle_cb (gpointer data)
{
	ECalErrorData *error_data = data;
	
	g_signal_emit (G_OBJECT (error_data->ecal), e_cal_signals[BACKEND_ERROR], 0, error_data->message);

	g_object_unref (error_data->ecal);
	g_free (error_data->message);
	g_free (error_data);
	
	return FALSE;
}

/* Handle the error_occurred signal from the listener */
static void
backend_error_cb (DBusGProxy *proxy, const char *message, ECal *ecal)
{
	ECalErrorData *error_data;
	
	error_data = g_new0 (ECalErrorData, 1);

	error_data->ecal = g_object_ref (ecal);
	error_data->message = g_strdup (message);

	g_idle_add (backend_error_idle_cb, error_data);
}

/* TODO - For now, the policy of where each backend serializes its
 * attachment data is hardcoded below. Should this end up as a
 * gconf key set during the account creation  and fetched
 * from eds???
 */
static void
set_local_attachment_store (ECal *ecal)
{
	ECalPrivate *priv;
	char *mangled_uri;
	int i;
	
	priv = E_CAL_GET_PRIVATE (ecal);
	mangled_uri = g_strdup (priv->uri);
	/* mangle the URI to not contain invalid characters */
	for (i = 0; i < strlen (mangled_uri); i++) {
		switch (mangled_uri[i]) {
		case ':' :
		case '/' :
			mangled_uri[i] = '_';
		}
	}

	/* the file backend uses its uri as the attachment store*/
	if (g_str_has_prefix (priv->uri, "file://")) {
		priv->local_attachment_store = g_strdup (priv->uri);
	} else if (g_str_has_prefix (priv->uri, "groupwise://")) {
		/* points to the location of the cache*/
		priv->local_attachment_store = 
			g_strconcat ("file://", g_get_home_dir (), "/", ".evolution/cache/calendar",
				     "/", mangled_uri, NULL);
	} else if (g_str_has_prefix (priv->uri, "exchange://")) {
		priv->local_attachment_store = g_strdup_printf ("file://%s/.evolution/exchange/%s", 
					g_get_home_dir (), mangled_uri);
	} else if (g_str_has_prefix (priv->uri, "scalix://")) {
                priv->local_attachment_store = g_strdup_printf ("file://%s/.evolution/cache/scalix/%s/attach",
                                        g_get_home_dir (), mangled_uri);
        }

	g_free (mangled_uri);
}

/**
 * e_cal_new:
 * @source: An #ESource to be used for the client.
 * @type: Type of the client.
 *
 * Creates a new calendar client. This does not open the calendar itself,
 * for that, #e_cal_open or #e_cal_open_async needs to be called.
 *
 * Return value: A newly-created calendar client, or NULL if the client could
 * not be constructed because it could not contact the calendar server.
 **/
ECal *
e_cal_new (ESource *source, ECalSourceType type)
{
	ECal *ecal;
	ECalPrivate *priv;
	gchar *path;
	GError *error = NULL;
	
	if (!e_cal_activate (&error)) {
		g_warning("Cannot activate ECal: %s\n", error->message);
		g_error_free (error);
		return NULL;
	}

	ecal = g_object_new (E_TYPE_CAL, NULL);
	priv = E_CAL_GET_PRIVATE (ecal);

	priv->source = g_object_ref (source);
	priv->uri = e_source_get_uri (source);
	priv->type = type;
	
	if (!org_gnome_evolution_dataserver_calendar_CalFactory_get_cal (factory_proxy, e_source_get_uri (priv->source), convert_type (priv->type), &path, &error)) {
		g_warning("Cannot get cal from factory: %s", error->message);
		g_error_free (error);
		g_object_unref (ecal);
		return NULL;
	}

	g_printerr("Got path %s for new calendar\n", path);
	priv->proxy = dbus_g_proxy_new_for_name_owner (connection,
									"org.gnome.evolution.dataserver.Calendar", path,
									"org.gnome.evolution.dataserver.calendar.Cal",
									&error);
	
	if (!priv->proxy)
		return NULL;

	dbus_g_proxy_add_signal (priv->proxy, "auth_required", G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (priv->proxy, "auth_required", G_CALLBACK (auth_required_cb), ecal, NULL);
	dbus_g_proxy_add_signal (priv->proxy, "backend_error", G_TYPE_STRING, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (priv->proxy, "backend_error", G_CALLBACK (backend_error_cb), ecal, NULL);
	dbus_g_proxy_add_signal (priv->proxy, "readonly", G_TYPE_BOOLEAN, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (priv->proxy, "readonly", G_CALLBACK (readonly_cb), ecal, NULL);
	dbus_g_proxy_add_signal (priv->proxy, "mode", G_TYPE_INT, G_TYPE_INVALID);
	dbus_g_proxy_connect_signal (priv->proxy, "mode", G_CALLBACK (mode_cb), ecal, NULL);
	
	/* Set the local attachment store path for the calendar */
	/* TODO: What does this do? */
	set_local_attachment_store (ecal);

	return ecal;
}

/**
 * e_cal_new_from_uri:
 * @uri: The URI pointing to the calendar to open.
 * @type: Type of the client.
 *
 * Creates a new calendar client. This does not open the calendar itself,
 * for that, #e_cal_open or #e_cal_open_async needs to be called.
 *
 * Return value: A newly-created calendar client, or NULL if the client could
 * not be constructed because it could not contact the calendar server.
 **/
ECal *
e_cal_new_from_uri (const gchar *uri, ECalSourceType type)
{
	ESource *source;
	ECal *cal;

	source = e_source_new_with_absolute_uri ("", uri);
	cal = e_cal_new (source, type);

	g_object_unref (source);

	return cal;
}

/**
 * e_cal_new_system_calendar:
 *
 * Create a calendar client for the system calendar, which should always be present in
 * all Evolution installations. This does not open the calendar itself,
 * for that, #e_cal_open or #e_cal_open_async needs to be called.
 *
 * Return value: A newly-created calendar client, or NULL if the client could
 * not be constructed.
 */
ECal *
e_cal_new_system_calendar (void)
{
	ECal *ecal;
	char *filename;
	char *uri;

	filename = g_build_filename (g_get_home_dir (),
				     ".evolution/calendar/local/system",
				     NULL);
	uri = g_filename_to_uri (filename, NULL, NULL);
	g_free (filename);
	ecal = e_cal_new_from_uri (uri, E_CAL_SOURCE_TYPE_EVENT);
	g_free (uri);

	return ecal;
}

/**
 * e_cal_new_system_tasks:
 *
 * Create a calendar client for the system task list, which should always be present in
 * all Evolution installations. This does not open the tasks list itself,
 * for that, #e_cal_open or #e_cal_open_async needs to be called.
 *
 * Return value: A newly-created calendar client, or NULL if the client could
 * not be constructed.
 */
ECal *
e_cal_new_system_tasks (void)
{
	ECal *ecal;
	char *filename;
	char *uri;

	filename = g_build_filename (g_get_home_dir (),
				     ".evolution/tasks/local/system",
				     NULL);
	uri = g_filename_to_uri (filename, NULL, NULL);
	g_free (filename);
	ecal = e_cal_new_from_uri (uri, E_CAL_SOURCE_TYPE_TODO);
	g_free (uri);

	return ecal;
}

/**
 * e_cal_new_system_memos:
 *
 * Create a calendar client for the system memos, which should always be present
 * in all Evolution installations. This does not open the memos itself, for
 * that, #e_cal_open or #e_cal_open_async needs to be called.
 *
 * Return value: A newly-created calendar client, or NULL if the client could
 * not be constructed.
 */
ECal *
e_cal_new_system_memos (void)
{
	ECal *ecal;
	char *uri;
	char *filename;

	filename = g_build_filename (g_get_home_dir (),
				     ".evolution/memos/local/system",
				     NULL);
	uri = g_filename_to_uri (filename, NULL, NULL);
	g_free (filename);
	ecal = e_cal_new_from_uri (uri, E_CAL_SOURCE_TYPE_JOURNAL);
	g_free (uri);

	return ecal;
}

/**
 * e_cal_set_auth_func
 * @ecal: A calendar client.
 * @func: The authentication function
 * @data: User data to be used when calling the authentication function
 *
 * Sets the given authentication function on the calendar client. This
 * function will be called any time the calendar server needs a
 * password for an operation associated with the calendar and should
 * be supplied before any calendar is opened.
 *
 * When a calendar is opened asynchronously, the open function is
 * processed in a concurrent thread.  This means that the
 * authentication function will also be called from this thread.  As
 * such, the authentication callback cannot directly call any
 * functions that must be called from the main thread.  For example
 * any Gtk+ related functions, which must be proxied synchronously to
 * the main thread by the callback.
 *
 * The authentication function has the following signature
 * (ECalAuthFunc):
 *	char * auth_func (ECal *ecal,
 *			  const gchar *prompt,
 *			  const gchar *key,
 *			  gpointer user_data)
 */
void
e_cal_set_auth_func (ECal *ecal, ECalAuthFunc func, gpointer data)
{
	ECalPrivate *priv;
	
	g_return_if_fail (ecal != NULL);
	g_return_if_fail (E_IS_CAL (ecal));

	priv = E_CAL_GET_PRIVATE (ecal);
	
	priv->auth_func = func;
	priv->auth_user_data = data;
}

static char *
get_host_from_uri (const char *uri)
{
	char *at = strchr (uri, '@');
	char *slash;
	int length;

	if (!at)
		return NULL;
	at++; /* Parse over the @ symbol */
	slash = strchr (at, '/');
	if (!slash)
		return NULL;
	slash --; /* Walk back */

	length = slash - at;

	return g_strndup (at, length);
}

static char *
build_pass_key (ESource *source)
{
	const char *base_uri, *user_name, *auth_type, *rel_uri;
	char *uri, *new_uri, *host_name;
	ESourceGroup *es_grp;

	es_grp = e_source_peek_group (source);
	base_uri = e_source_group_peek_base_uri (es_grp);
	if (base_uri) {
		user_name = e_source_get_property (source, "username");
		auth_type = e_source_get_property (source, "auth-type");
		rel_uri = e_source_peek_relative_uri (source);
		host_name = get_host_from_uri (rel_uri);
		if (host_name) {
			new_uri = g_strdup_printf ("%s%s;%s@%s/", base_uri, user_name, auth_type, host_name);
			g_free (host_name);

			return new_uri;
		}
	}

	uri = e_source_get_uri (source);

	return uri;
}

static void
async_signal_idle_cb (DBusGProxy *proxy, GError *error, gpointer user_data)
{
	ECal *ecal;
	ECalendarStatus status;
	
	ecal = E_CAL (user_data);
	if (error)
		status = get_status_from_error (error);
	else
		status = E_CALENDAR_STATUS_OK;

	g_signal_emit (G_OBJECT (ecal), e_cal_signals[CAL_OPENED], 0, status);
}

static gboolean
open_calendar (ECal *ecal, gboolean only_if_exists, GError **error, ECalendarStatus *status, gboolean needs_auth, gboolean async)
{
	ECalPrivate *priv;
	const char *username = NULL, *auth_type = NULL;
	char *password = NULL;
	
	e_return_error_if_fail (ecal != NULL, E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);
	
	/* TODO: Do we really want to get rid of the mutex's? */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (!needs_auth && priv->load_state == E_CAL_LOAD_LOADED) {
/*		g_mutex_unlock (ecal->priv->mutex);*/
		return TRUE;
	}
	
/*	g_mutex_unlock (priv->mutex);*/

	/* see if the backend needs authentication */
	if ( (priv->mode !=  CAL_MODE_LOCAL) && e_source_get_property (priv->source, "auth")) {
		char *prompt, *key;
		const char *parent_user;

		priv->load_state = E_CAL_LOAD_AUTHENTICATING;

		if (priv->auth_func == NULL) {
			priv->load_state = E_CAL_LOAD_NOT_LOADED;
			*status = E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED;
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED, error);
		}

		username = e_source_get_property (priv->source, "username");
		if (!username) {
			priv->load_state = E_CAL_LOAD_NOT_LOADED;
			*status = E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED;
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED, error);
		}

		/* actually ask the client for authentication */
		parent_user = e_source_get_property (priv->source, "parent_id_name");
		if (parent_user) {			
			/* FIXME: Try to get the parent uri so that we dont have to ask the password again */
			prompt = g_strdup_printf (_("Enter password for %s to enable proxy for user %s"), e_source_peek_name (priv->source), parent_user);

		} else {
			prompt = g_strdup_printf (_("Enter password for %s (user %s)"),
					e_source_peek_name (priv->source), username);
		}
		
		auth_type = e_source_get_property (priv->source, "auth-type");
		if (auth_type)
			key = build_pass_key (priv->source);
		else
			key = e_source_get_uri (priv->source);
		
		if (!key) {
			priv->load_state = E_CAL_LOAD_NOT_LOADED;
			*status = E_CALENDAR_STATUS_URI_NOT_LOADED;
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED, error);
		}
			
		password = priv->auth_func (ecal, prompt, key, priv->auth_user_data);

		if (!password) {
			priv->load_state = E_CAL_LOAD_NOT_LOADED;
			*status = E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED; 
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED, error);
		}

		g_free (prompt);
		g_free (key);
	}

	priv->load_state = E_CAL_LOAD_LOADING;

	*status = E_CALENDAR_STATUS_OK;
	if (!async ) {
		if (!org_gnome_evolution_dataserver_calendar_Cal_open (priv->proxy, only_if_exists, username ? username : "", password ? password : "", error))
			*status = E_CALENDAR_STATUS_CORBA_EXCEPTION;
	} else {
		if (!org_gnome_evolution_dataserver_calendar_Cal_open_async (priv->proxy, only_if_exists, username ? username : "", password ? password : "", async_signal_idle_cb, ecal))
			*status = E_CALENDAR_STATUS_CORBA_EXCEPTION;
	}
	
	if (password)
		g_free (password);

	if (*status == E_CALENDAR_STATUS_OK) {
		GError *error = NULL;
		priv->load_state = E_CAL_LOAD_LOADED;
		org_gnome_evolution_dataserver_calendar_Cal_is_read_only (priv->proxy, &error);
	} else
		priv->load_state = E_CAL_LOAD_NOT_LOADED;

	E_CALENDAR_CHECK_STATUS (*status, error);
}

/**
 * e_cal_open
 * @ecal: A calendar client.
 * @only_if_exists: FALSE if the calendar should be opened even if there
 * was no storage for it, i.e. to create a new calendar or load an existing
 * one if it already exists.  TRUE if it should only try to load calendars
 * that already exist.
 * @error: Placeholder for error information.
 *
 * Makes a calendar client initiate a request to open a calendar.  The calendar
 * client will emit the "cal_opened" signal when the response from the server is
 * received.
 *
 * Return value: TRUE on success, FALSE on failure to issue the open request.
 **/
gboolean
e_cal_open (ECal *ecal, gboolean only_if_exists, GError **error)
{
	ECalendarStatus status;
	gboolean result;
	
	result = open_calendar (ecal, only_if_exists, error, &status, FALSE, FALSE);
	g_signal_emit (G_OBJECT (ecal), e_cal_signals[CAL_OPENED], 0, status);

	return result;
}

/**
 * e_cal_open_async:
 * @ecal: A calendar client.
 * @only_if_exists: If TRUE, then only open the calendar if it already
 * exists.  If FALSE, then create a new calendar if it doesn't already
 * exist.
 * 
 * Open the calendar asynchronously.  The calendar will emit the
 * "cal_opened" signal when the operation has completed.
 * 
 * Because this operation runs in another thread, any authentication
 * callback set on the calendar will be called from this other thread.
 * See #e_cal_set_auth_func() for details.
 **/
void
e_cal_open_async (ECal *ecal, gboolean only_if_exists)
{
	ECalPrivate *priv;
	GError *error = NULL;
	ECalendarStatus status;
	
	g_return_if_fail (ecal != NULL);
	g_return_if_fail (E_IS_CAL (ecal));

	priv = E_CAL_GET_PRIVATE (ecal);
	
	switch (priv->load_state) {
	case E_CAL_LOAD_AUTHENTICATING :
	case E_CAL_LOAD_LOADING :
		g_signal_emit (G_OBJECT (ecal), e_cal_signals[CAL_OPENED], 0, E_CALENDAR_STATUS_BUSY);
		return;
	case E_CAL_LOAD_LOADED :
		g_signal_emit (G_OBJECT (ecal), e_cal_signals[CAL_OPENED], 0, E_CALENDAR_STATUS_OK);
		return;
	default:
		/* ignore everything else */
		break;
	}

	open_calendar (ecal, only_if_exists, &error, &status, FALSE, TRUE);
}

/**
 * e_cal_remove:
 * @ecal: A calendar client.
 * @error: Placeholder for error information.
 *
 * Removes a calendar.
 *
 * Return value: TRUE if the calendar was removed, FALSE if there was an error.
 */
gboolean 
e_cal_remove (ECal *ecal, GError **error)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: Investigate if mutex's are necessary */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (!org_gnome_evolution_dataserver_calendar_Cal_remove (priv->proxy, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	return TRUE;
}

#if 0
/* Builds an URI list out of a CORBA string sequence */
static GList *
build_uri_list (GNOME_Evolution_Calendar_StringSeq *seq)
{
	GList *uris = NULL;
	int i;
	
	for (i = 0; i < seq->_length; i++)
		uris = g_list_prepend (uris, g_strdup (seq->_buffer[i]));

	return uris;
}
#endif

/**
 * e_cal_uri_list:
 * @ecal: A calendar client.
 * @mode: Mode of the URIs to get.
 *
 * Retrieves a list of all calendar clients for the given mode.
 *
 * Return value: list of uris.
 */
GList *
e_cal_uri_list (ECal *ecal, CalMode mode)
{
#if 0
	ECalPrivate *priv;
	GNOME_Evolution_Calendar_StringSeq *uri_seq;
	GList *uris = NULL;	
	CORBA_Environment ev;
	GList *f;

	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	priv = ecal->priv;

	for (f = priv->factories; f; f = f->next) {
		CORBA_exception_init (&ev);
		uri_seq = GNOME_Evolution_Calendar_CalFactory_uriList (f->data, mode, &ev);

		if (BONOBO_EX (&ev)) {
			g_message ("e_cal_uri_list(): request failed");

			/* free memory and return */
			g_list_foreach (uris, (GFunc) g_free, NULL);
			g_list_free (uris);
			uris = NULL;
			break;
		}
		else {
			uris = g_list_concat (uris, build_uri_list (uri_seq));
			CORBA_free (uri_seq);
		}
	
		CORBA_exception_free (&ev);
	}
	
	return uris;	
#endif

	return NULL;
}

/**
 * e_cal_get_source_type:
 * @ecal: A calendar client.
 *
 * Gets the type of the calendar client.
 *
 * Return value: an #ECalSourceType value corresponding to the type
 * of the calendar client.
 */
ECalSourceType
e_cal_get_source_type (ECal *ecal)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, E_CAL_SOURCE_TYPE_LAST);
	g_return_val_if_fail (E_IS_CAL (ecal), E_CAL_SOURCE_TYPE_LAST);

	priv = E_CAL_GET_PRIVATE (ecal);

	return priv->type;
}

/**
 * e_cal_get_load_state:
 * @ecal: A calendar client.
 * 
 * Queries the state of loading of a calendar client.
 * 
 * Return value: A #ECalLoadState value indicating whether the client has
 * not been loaded with #e_cal_open yet, whether it is being
 * loaded, or whether it is already loaded.
 **/
ECalLoadState
e_cal_get_load_state (ECal *ecal)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, E_CAL_LOAD_NOT_LOADED);
	g_return_val_if_fail (E_IS_CAL (ecal), E_CAL_LOAD_NOT_LOADED);

	priv = E_CAL_GET_PRIVATE (ecal);
	return priv->load_state;
}

/**
 * e_cal_get_source:
 * @ecal: A calendar client.
 * 
 * Queries the source that is open in a calendar client.
 * 
 * Return value: The source of the calendar that is already loaded or is being
 * loaded, or NULL if the ecal has not started a load request yet.
 **/
ESource *
e_cal_get_source (ECal *ecal)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL (ecal), NULL);

	priv = E_CAL_GET_PRIVATE (ecal);
	return priv->source;
}

/**
 * e_cal_get_uri:
 * @ecal: A calendar client.
 * 
 * Queries the URI that is open in a calendar client.
 * 
 * Return value: The URI of the calendar that is already loaded or is being
 * loaded, or NULL if the client has not started a load request yet.
 **/
const char *
e_cal_get_uri (ECal *ecal)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL (ecal), NULL);

	priv = E_CAL_GET_PRIVATE (ecal);
	return priv->uri;
}

/**
 * e_cal_get_local_attachment_store
 * @ecal: A calendar client.
 * 
 * Queries the URL where the calendar attachments are
 * serialized in the local filesystem. This enable clients
 * to operate with the reference to attachments rather than the data itself
 * unless it specifically uses the attachments for open/sending
 * operations.
 *
 * Return value: The URL where the attachments are serialized in the
 * local filesystem.
 **/
const char *
e_cal_get_local_attachment_store (ECal *ecal)
{
	ECalPrivate *priv;
	
	g_return_val_if_fail (ecal != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL (ecal), NULL);

	priv = E_CAL_GET_PRIVATE (ecal);
	return (const char *)priv->local_attachment_store;
}

/**
 * e_cal_is_read_only:
 * @ecal: A calendar client.
 * @read_only: Return value for read only status.
 * @error: Placeholder for error information.
 *
 * Queries whether the calendar client can perform modifications
 * on the calendar or not. Whether the backend is read only or not
 * is specified, on exit, in the @read_only argument.
 *
 * Return value: TRUE if the call was successful, FALSE if there was an error.
 */
gboolean
e_cal_is_read_only (ECal *ecal, gboolean *read_only, GError **error)
{
	ECalPrivate *priv;
	
	if (!(ecal && E_IS_CAL (ecal)))
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_INVALID_ARG, error);
	
	priv = E_CAL_GET_PRIVATE (ecal);
	*read_only = priv->read_only;
	
	return TRUE;
}

/**
 * e_cal_get_cal_address:
 * @ecal: A calendar client.
 * @cal_address: Return value for address information.
 * @error: Placeholder for error information.
 *
 * Queries the calendar address associated with a calendar client.
 * 
 * Return value: TRUE if the operation was successful, FALSE if there
 * was an error.
 **/
gboolean
e_cal_get_cal_address (ECal *ecal, char **cal_address, GError **error)
{
	ECalPrivate *priv;
	
	if (!(ecal && E_IS_CAL (ecal)))
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_INVALID_ARG, error);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->cal_address == NULL) { 
		if (priv->load_state != E_CAL_LOAD_LOADED) {
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
		}
	
		if (!org_gnome_evolution_dataserver_calendar_Cal_get_cal_address (priv->proxy, cal_address, error)) {
			E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
		}
	} else {
		*cal_address = g_strdup (priv->cal_address);
	}

	return TRUE;
}

/**
 * e_cal_get_alarm_email_address:
 * @ecal: A calendar client.
 * @alarm_address: Return value for alarm address.
 * @error: Placeholder for error information.
 *
 * Queries the address to be used for alarms in a calendar client.
 *
 * Return value: TRUE if the operation was successful, FALSE if there was
 * an error while contacting the backend.
 */
gboolean
e_cal_get_alarm_email_address (ECal *ecal, char **alarm_address, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_alarm_email_address (priv->proxy, alarm_address, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}
	
	return TRUE;
}

/**
 * e_cal_get_ldap_attribute:
 * @ecal: A calendar client.
 * @ldap_attribute: Return value for the LDAP attribute.
 * @error: Placeholder for error information.
 *
 * Queries the LDAP attribute for a calendar client.
 *
 * Return value: TRUE if the call was successful, FALSE if there was an
 * error contacting the backend.
 */
gboolean
e_cal_get_ldap_attribute (ECal *ecal, char **ldap_attribute, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_ldap_attribute (priv->proxy, ldap_attribute, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}
	
	return TRUE;
}

static gboolean
load_static_capabilities (ECal *ecal, GError **error) 
{
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);

	if (priv->capabilities)
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_scheduling_information (priv->proxy, &priv->capabilities, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}
	
	return TRUE;
}

static gboolean
check_capability (ECal *ecal, const char *cap) 
{
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);

	/* FIXME Check result */
	load_static_capabilities (ecal, NULL);
	if (priv->capabilities && strstr (priv->capabilities, cap))
		return TRUE;
	
	return FALSE;
}

/**
 * e_cal_get_one_alarm_only:
 * @ecal: A calendar client.
 *
 * Checks if a calendar supports only one alarm per component.
 *
 * Return value: TRUE if the calendar allows only one alarm, FALSE otherwise.
 */
gboolean
e_cal_get_one_alarm_only (ECal *ecal)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (ecal && E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, CAL_STATIC_CAPABILITY_ONE_ALARM_ONLY);
}

/**
 * e_cal_get_organizer_must_attend:
 * @ecal: A calendar client.
 *
 * Checks if a calendar forces organizers of meetings to be also attendees.
 *
 * Return value: TRUE if the calendar forces organizers to attend meetings,
 * FALSE otherwise.
 */
gboolean 
e_cal_get_organizer_must_attend (ECal *ecal)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, CAL_STATIC_CAPABILITY_ORGANIZER_MUST_ATTEND);
}

/**
 * e_cal_get_recurrences_no_master:
 * @ecal: A calendar client.
 *
 * Checks if the calendar has a master object for recurrences.
 *
 * Return value: TRUE if the calendar has a master object for recurrences,
 * FALSE otherwise.
 */
gboolean 
e_cal_get_recurrences_no_master (ECal *ecal)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, CAL_STATIC_CAPABILITY_RECURRENCES_NO_MASTER);
}

/**
 * e_cal_get_static_capability:
 * @ecal: A calendar client.
 * @cap: Name of the static capability to check.
 *
 * Queries the calendar for static capabilities.
 *
 * Return value: TRUE if the capability is supported, FALSE otherwise.
 */
gboolean
e_cal_get_static_capability (ECal *ecal, const char *cap)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, cap);
}

/**
 * e_cal_get_save_schedules:
 * @ecal: A calendar client.
 *
 * Checks whether the calendar saves schedules.
 *
 * Return value: TRUE if it saves schedules, FALSE otherwise.
 */
gboolean 
e_cal_get_save_schedules (ECal *ecal)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, CAL_STATIC_CAPABILITY_SAVE_SCHEDULES);
}

/**
 * e_cal_get_organizer_must_accept:
 * @ecal: A calendar client.
 *
 * Checks whether a calendar requires organizer to accept their attendance to
 * meetings.
 *
 * Return value: TRUE if the calendar requires organizers to accept, FALSE
 * otherwise.
 */
gboolean 
e_cal_get_organizer_must_accept (ECal *ecal)
{
	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	return check_capability (ecal, CAL_STATIC_CAPABILITY_ORGANIZER_MUST_ACCEPT);
}

/**
 * e_cal_set_mode:
 * @ecal: A calendar client.
 * @mode: Mode to switch to.
 *
 * Switches online/offline mode on the calendar.
 *
 * Return value: TRUE if the switch was successful, FALSE if there was an error.
 */
gboolean
e_cal_set_mode (ECal *ecal, CalMode mode)
{
	ECalPrivate *priv;
	GError *error = NULL;

	g_return_val_if_fail (ecal != NULL, -1);
	g_return_val_if_fail (E_IS_CAL (ecal), -1);

	priv = E_CAL_GET_PRIVATE (ecal);
	g_return_val_if_fail (priv->load_state == E_CAL_LOAD_LOADED, -1);

	if (!org_gnome_evolution_dataserver_calendar_Cal_set_mode (priv->proxy, mode, &error)) {
		g_printerr("%s: %s\n", __FUNCTION__, error->message);
		g_error_free (error);
		return FALSE;
	}
	
	return TRUE;
}


/* This is used in the callback which fetches all the timezones needed for an
   object. */
typedef struct _ECalGetTimezonesData ECalGetTimezonesData;
struct _ECalGetTimezonesData {
	ECal *ecal;

	/* This starts out at E_CALENDAR_STATUS_OK. If an error occurs this
	   contains the last error. */
	ECalendarStatus status;
};

/**
 * e_cal_get_default_object:
 * @ecal: A calendar client.
 * @icalcomp: Return value for the default object.
 * @error: Placeholder for error information.
 *
 * Retrives an #icalcomponent from the backend that contains the default
 * values for properties needed.
 *
 * Return value: TRUE if the call was successful, FALSE otherwise.
 */
gboolean
e_cal_get_default_object (ECal *ecal, icalcomponent **icalcomp, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	gchar *object;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_default_object (priv->proxy, &object, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	*icalcomp = NULL;
	if (object) {
		*icalcomp = icalparser_parse_string (object);
		g_free (object);
		
		if (!(*icalcomp))
			status = E_CALENDAR_STATUS_INVALID_OBJECT;
		else
			status = E_CALENDAR_STATUS_OK;
			
		E_CALENDAR_CHECK_STATUS (status, error);
	} else
		status = E_CALENDAR_STATUS_OTHER_ERROR;

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_get_attachments_for_comp:
 * @ecal: A calendar client.
 * @uid: Unique identifier for a calendar component.
 * @rid: Recurrence identifier.
 * @list: Return the list of attachment uris.
 * @error: Placeholder for error information.
 *
 * Queries a calendar for a calendar component object based on its unique
 * identifier and gets the attachments for the component.
 *
 * Return value: TRUE if the call was successful, FALSE otherwise.
 **/
gboolean
e_cal_get_attachments_for_comp (ECal *ecal, const char *uid, const char *rid, GSList **list, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	char **list_array;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_attachment_list (priv->proxy, uid, rid ? rid: "", &list_array, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	if (list_array) {
		char **string;
		for (string = list_array; *string; string++) {
			*list = g_slist_append (*list, g_strdup (*string));
		}
		g_strfreev (list_array);
		status = E_CALENDAR_STATUS_OK;
	} else
		status = E_CALENDAR_STATUS_OTHER_ERROR;

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_get_object:
 * @ecal: A calendar client.
 * @uid: Unique identifier for a calendar component.
 * @rid: Recurrence identifier.
 * @icalcomp: Return value for the calendar component object.
 * @error: Placeholder for error information.
 *
 * Queries a calendar for a calendar component object based on its unique
 * identifier.
 *
 * Return value: TRUE if the call was successful, FALSE otherwise.
 **/
gboolean
e_cal_get_object (ECal *ecal, const char *uid, const char *rid, icalcomponent **icalcomp, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	gchar *object;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_object (priv->proxy, uid, rid ? rid : "", &object, error)) {
		g_warning ("%s failed with uid %s, rid %s", __FUNCTION__, uid, rid ? rid : "");
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	status = E_CALENDAR_STATUS_OK;
   {
		icalcomponent *tmp_icalcomp;
		icalcomponent_kind kind;

                tmp_icalcomp = icalparser_parse_string (object);
		if (!tmp_icalcomp) {
			status = E_CALENDAR_STATUS_INVALID_OBJECT;
			*icalcomp = NULL;
		} else {
			kind = icalcomponent_isa (tmp_icalcomp);
			if ((kind == ICAL_VEVENT_COMPONENT && priv->type == E_CAL_SOURCE_TYPE_EVENT) ||
			    (kind == ICAL_VTODO_COMPONENT && priv->type == E_CAL_SOURCE_TYPE_TODO)) {
				*icalcomp = icalcomponent_new_clone (tmp_icalcomp);
			} else if (kind == ICAL_VCALENDAR_COMPONENT) {
				icalcomponent *subcomp = NULL;

				switch (priv->type) {
				case E_CAL_SOURCE_TYPE_EVENT :
					subcomp = icalcomponent_get_first_component (tmp_icalcomp, ICAL_VEVENT_COMPONENT);
					break;
				case E_CAL_SOURCE_TYPE_TODO :
					subcomp = icalcomponent_get_first_component (tmp_icalcomp, ICAL_VTODO_COMPONENT);
					break;
				case E_CAL_SOURCE_TYPE_JOURNAL :
					subcomp = icalcomponent_get_first_component (tmp_icalcomp, ICAL_VJOURNAL_COMPONENT);
					break;
				default:
					/* ignore everything else */
					break;
				}

				/* we are only interested in the first component */
				if (subcomp)
					*icalcomp = icalcomponent_new_clone (subcomp);
			}

			icalcomponent_free (tmp_icalcomp);
		}
	}
	g_free (object);

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_get_objects_for_uid:
 * @ecal: A calendar client.
 * @uid: Unique identifier for a calendar component.
 * @objects: Return value for the list of objects obtained from the backend.
 * @error: Placeholder for error information.
 *
 * Queries a calendar for all calendar components with the given unique
 * ID. This will return any recurring event and all its detached recurrences.
 * For non-recurring events, it will just return the object with that ID.
 *
 * Return value: TRUE if the call was successful, FALSE otherwise.
 **/
gboolean
e_cal_get_objects_for_uid (ECal *ecal, const char *uid, GList **objects, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	gchar *object;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);
	*objects = NULL;

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_object (priv->proxy, uid, "", &object, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	status = E_CALENDAR_STATUS_OK;
	{
		icalcomponent *icalcomp;
		icalcomponent_kind kind;

		icalcomp = icalparser_parse_string (object);
		if (!icalcomp) {
			status = E_CALENDAR_STATUS_INVALID_OBJECT;
			*objects = NULL;
		} else {
			ECalComponent *comp;

			kind = icalcomponent_isa (icalcomp);
			if ((kind == ICAL_VEVENT_COMPONENT && priv->type == E_CAL_SOURCE_TYPE_EVENT) ||
			    (kind == ICAL_VTODO_COMPONENT && priv->type == E_CAL_SOURCE_TYPE_TODO) ||
			    (kind == ICAL_VJOURNAL_COMPONENT && priv->type == E_CAL_SOURCE_TYPE_JOURNAL)) {
				comp = e_cal_component_new ();
				e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (icalcomp));
				*objects = g_list_append (NULL, comp);
			} else if (kind == ICAL_VCALENDAR_COMPONENT) {
				icalcomponent *subcomp;
				icalcomponent_kind kind_to_find;

				switch (priv->type) {
				case E_CAL_SOURCE_TYPE_TODO :
					kind_to_find = ICAL_VTODO_COMPONENT;
					break;
				case E_CAL_SOURCE_TYPE_JOURNAL :
					kind_to_find = ICAL_VJOURNAL_COMPONENT;
					break;
				case E_CAL_SOURCE_TYPE_EVENT :
				default:
					kind_to_find = ICAL_VEVENT_COMPONENT;
					break;
				}

				*objects = NULL;
				subcomp = icalcomponent_get_first_component (icalcomp, kind_to_find);
				while (subcomp) {
					comp = e_cal_component_new ();
					e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (subcomp));
					*objects = g_list_append (*objects, comp);
					subcomp = icalcomponent_get_next_component (icalcomp, kind_to_find);
				}
			}

			icalcomponent_free (icalcomp);
		}
	}
	g_free (object);

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_resolve_tzid_cb:
 * @tzid: ID of the timezone to resolve.
 * @data: Closure data for the callback.
 *
 * Resolves TZIDs for the recurrence generator.
 *
 * Return value: The timezone identified by the @tzid argument, or %NULL if
 * it could not be found.
 */
icaltimezone*
e_cal_resolve_tzid_cb (const char *tzid, gpointer data)
{
	ECal *ecal;
	icaltimezone *zone = NULL;

	g_return_val_if_fail (data != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL (data), NULL);
	
	ecal = E_CAL (data);

	/* FIXME: Handle errors. */
	e_cal_get_timezone (ecal, tzid, &zone, NULL);

	return zone;
}

/**
 * e_cal_get_changes:
 * @ecal: A calendar client.
 * @change_id: ID to use for comparing changes.
 * @changes: Return value for the list of changes.
 * @error: Placeholder for error information.
 *
 * Returns a list of changes made to the calendar since a specific time. That time
 * is identified by the @change_id argument, which is used by the backend to
 * compute the changes done.
 *
 * Return value: TRUE if the call was successful, FALSE otherwise.
 */
gboolean
e_cal_get_changes (ECal *ecal, const char *change_id, GList **changes, GError **error)
{
	ECalPrivate *priv;
	char **additions, **modifications, **removals;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	
	e_return_error_if_fail (change_id, E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);
	
	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_changes (priv->proxy, change_id, &additions, &modifications, &removals, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	/* TODO: Be more elegant and split this into a function */
	/* Mostly copied from the old e-cal-listener.c */
	*changes = NULL;
	if ((additions)&&(modifications)&&(removals)) {
		int i;
		char **list = NULL, **l;
		ECalChangeType change_type;
		icalcomponent *icalcomp;
		ECalChange *change;
		
		for (i = 0; i < 3; i++) {
			switch (i) {
			case 0:
				change_type = E_CAL_CHANGE_ADDED;
				list = additions;
				break;
			case 1:
				change_type = E_CAL_CHANGE_MODIFIED;
				list = modifications;
				break;
			case 2:
				change_type = E_CAL_CHANGE_DELETED;
				list = removals;
			}
			
			for (l = list; *l; l++) {
				icalcomp = icalparser_parse_string (*l);
				if (!icalcomp)
					continue;
				change = g_new (ECalChange, 1);
				change->comp = e_cal_component_new ();
				if (!e_cal_component_set_icalcomponent (change->comp, icalcomp)) {
					icalcomponent_free (icalcomp);
					g_object_unref (G_OBJECT (change->comp));
					g_free (change);
					continue;
				}
				change->type = change_type;
				*changes = g_list_append (*changes, change);
			}
		}
	}
	else
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OTHER_ERROR, error);

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

/**
 * e_cal_free_change_list:
 * @list: List of changes to be freed.
 *
 * Free a list of changes as returned by #e_cal_get_changes.
 */
void
e_cal_free_change_list (GList *list)
{
	ECalChange *c;
	GList *l;

	for (l = list; l; l = l->next) {
		c = l->data;

		g_assert (c != NULL);
		g_assert (c->comp != NULL);

		g_object_unref (G_OBJECT (c->comp));
		g_free (c);
	}

	g_list_free (list);
}

/**
 * e_cal_get_object_list:
 * @ecal: A calendar client.
 * @query: Query string.
 * @objects: Return value for list of objects.
 * @error: Placeholder for error information.
 * 
 * Gets a list of objects from the calendar that match the query specified
 * by the @query argument. The objects will be returned in the @objects
 * argument, which is a list of #icalcomponent. When done, this list
 * should be freed by using the #e_cal_free_object_list function.
 * 
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 **/
gboolean
e_cal_get_object_list (ECal *ecal, const char *query, GList **objects, GError **error)
{
	ECalPrivate *priv;
	char **object_array;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	
	e_return_error_if_fail (query, E_CALENDAR_STATUS_INVALID_ARG);
	
	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_object_list (priv->proxy, query, &object_array, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	if (object_array) {
		icalcomponent *comp;
		char **object;
		for (object = object_array; *object; object++) {
			comp = icalcomponent_new_from_string (*object);
			if (!comp) continue;
			*objects = g_list_prepend (*objects, comp);
		}
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
	}
	else
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OTHER_ERROR, error);		
}

/**
 * e_cal_get_object_list_as_comp:
 * @ecal: A calendar client.
 * @query: Query string.
 * @objects: Return value for list of objects.
 * @error: Placeholder for error information.
 * 
 * Gets a list of objects from the calendar that match the query specified
 * by the @query argument. The objects will be returned in the @objects
 * argument, which is a list of #ECalComponent.
 * 
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_get_object_list_as_comp (ECal *ecal, const char *query, GList **objects, GError **error)
{
	GList *ical_objects = NULL;
	GList *l;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	
	e_return_error_if_fail (query, E_CALENDAR_STATUS_INVALID_ARG);	
	e_return_error_if_fail (objects, E_CALENDAR_STATUS_INVALID_ARG);	
	
	if (!e_cal_get_object_list (ecal, query, &ical_objects, error))
		return FALSE;
		
	*objects = NULL;
	for (l = ical_objects; l; l = l->next) {
		ECalComponent *comp;
			
		comp = e_cal_component_new ();
		e_cal_component_set_icalcomponent (comp, l->data);
		*objects = g_list_prepend (*objects, comp);
	}
			
	g_list_free (ical_objects);

	return TRUE;
}

/**
 * e_cal_free_object_list:
 * @objects: List of objects to be freed.
 *
 * Frees a list of objects as returned by #e_cal_get_object_list.
 */
void 
e_cal_free_object_list (GList *objects)
{
	GList *l;

	for (l = objects; l; l = l->next)
		icalcomponent_free (l->data);

	g_list_free (objects);
}

static GList *
build_free_busy_list (const char **seq)
{
	GList *list = NULL;
	int i;

	/* Create the list in reverse order */
	for (i = 0; seq[i]; i++) {
		ECalComponent *comp;
		icalcomponent *icalcomp;
		icalcomponent_kind kind;

		icalcomp = icalcomponent_new_from_string ((char*)seq[i]);
		if (!icalcomp)
			continue;

		kind = icalcomponent_isa (icalcomp);
		if (kind == ICAL_VFREEBUSY_COMPONENT) {
			comp = e_cal_component_new ();
			if (!e_cal_component_set_icalcomponent (comp, icalcomp)) {
				icalcomponent_free (icalcomp);
				g_object_unref (G_OBJECT (comp));
				continue;
			}
			
			list = g_list_append (list, comp);
		} else {
			icalcomponent_free (icalcomp);
		}
	}

	return list;
}

/**
 * e_cal_get_free_busy
 * @ecal: A calendar client.
 * @users: List of users to retrieve free/busy information for.
 * @start: Start time for query.
 * @end: End time for query.
 * @freebusy: Return value for VFREEBUSY objects.
 * @error: 
 *
 * Gets free/busy information from the calendar server.
 *
 * Returns: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_get_free_busy (ECal *ecal, GList *users, time_t start, time_t end,
		     GList **freebusy, GError **error)
{
	ECalPrivate *priv;
	char **users_list;
	char **freebusy_array;
	GList *l;
	int i;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);
	
	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	users_list = g_new0 (char *, g_list_length (users) + 1);
	for (l = users, i = 0; l; l = l->next, i++)
		users_list[i] = g_strdup (l->data);

	if (!org_gnome_evolution_dataserver_calendar_Cal_get_free_busy (priv->proxy, (const char**)users_list, start, end, &freebusy_array, error)) {
		g_strfreev (users_list);
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);	
	}
	g_strfreev (users_list);
	
	if (freebusy_array) {
		*freebusy = build_free_busy_list ((const char**)freebusy_array);
		g_strfreev (freebusy_array);
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
	} else
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OTHER_ERROR, error);
}

struct comp_instance {
	ECalComponent *comp;
	time_t start;
	time_t end;
};

/* Called from cal_recur_generate_instances(); adds an instance to the list */
static gboolean
add_instance (ECalComponent *comp, time_t start, time_t end, gpointer data)
{
	GList **list;
	struct comp_instance *ci;
	struct icaltimetype itt, itt_start;
	icalcomponent *icalcomp;

	list = data;

	ci = g_new (struct comp_instance, 1);

	icalcomp = icalcomponent_new_clone (e_cal_component_get_icalcomponent (comp));
	itt_start = icalcomponent_get_dtstart (icalcomp);

	/* set the RECUR-ID for the instance */
	if (e_cal_util_component_has_recurrences (icalcomp)) {
		if (!(icalcomponent_get_first_property (icalcomp, ICAL_RECURRENCEID_PROPERTY))) {
			if (itt_start.zone)
				itt = icaltime_from_timet_with_zone (start, itt_start.is_date, itt_start.zone);
			else
				itt = icaltime_from_timet (start, itt_start.is_date);
			icalcomponent_set_recurrenceid (icalcomp, itt);
		}
	}

	/* add the instance to the list */
	ci->comp = e_cal_component_new ();
	e_cal_component_set_icalcomponent (ci->comp, icalcomp);
	
	ci->start = start;
	ci->end = end;

	*list = g_list_prepend (*list, ci);

	return TRUE;
}

/* Used from g_list_sort(); compares two struct comp_instance structures */
static gint
compare_comp_instance (gconstpointer a, gconstpointer b)
{
	const struct comp_instance *cia, *cib;
	time_t diff;

	cia = a;
	cib = b;

	diff = cia->start - cib->start;
	return (diff < 0) ? -1 : (diff > 0) ? 1 : 0;
}

static GList *
process_detached_instances (GList *instances, GList *detached_instances)
{
	struct comp_instance *ci, *cid;
	GList *dl, *unprocessed_instances = NULL;

	for (dl = detached_instances; dl != NULL; dl = dl->next) {
		GList *il;
		const char *uid;
		gboolean processed;
		ECalComponentRange recur_id, instance_recur_id;

		processed = FALSE;

		cid = dl->data;
		e_cal_component_get_uid (cid->comp, &uid);
		e_cal_component_get_recurid (cid->comp, &recur_id);

		/* search for coincident instances already expanded */
		for (il = instances; il != NULL; il = il->next) {
			const char *instance_uid;
			int cmp;

			ci = il->data;
			e_cal_component_get_uid (ci->comp, &instance_uid);
			e_cal_component_get_recurid (ci->comp, &instance_recur_id);
			if (strcmp (uid, instance_uid) == 0) {
				if (strcmp (e_cal_component_get_recurid_as_string (ci->comp),
					    e_cal_component_get_recurid_as_string (cid->comp)) == 0) {
					g_object_unref (ci->comp);
					ci->comp = g_object_ref (cid->comp);
					ci->start = cid->start;
					ci->end = cid->end;

					processed = TRUE;
				} else {
					cmp = icaltime_compare (*instance_recur_id.datetime.value,
								*recur_id.datetime.value);
					if ((recur_id.type == E_CAL_COMPONENT_RANGE_THISPRIOR && cmp <= 0) ||
					    (recur_id.type == E_CAL_COMPONENT_RANGE_THISFUTURE && cmp >= 0)) {
						ECalComponent *comp;

						comp = e_cal_component_new ();
						e_cal_component_set_icalcomponent (
							comp,
							icalcomponent_new_clone (e_cal_component_get_icalcomponent (cid->comp)));
						e_cal_component_set_recurid (comp, &instance_recur_id);

						/* replace the generated instances */
						g_object_unref (ci->comp);
						ci->comp = comp;
					}
				}
			}
		}

		if (!processed)
			unprocessed_instances = g_list_prepend (unprocessed_instances, cid);
	}

	/* add the unprocessed instances (ie, detached instances with no master object */
	while (unprocessed_instances != NULL) {
		cid = unprocessed_instances->data;
		ci = g_new0 (struct comp_instance, 1);
		ci->comp = g_object_ref (cid->comp);
		ci->start = cid->start;
		ci->end = cid->end;
		instances = g_list_append (instances, ci);

		unprocessed_instances = g_list_remove (unprocessed_instances, cid);
	}

	return instances;
}

static void
generate_instances (ECal *ecal, time_t start, time_t end, const char *uid,
		    ECalRecurInstanceFn cb, gpointer cb_data)
{
	GList *objects = NULL;
	GList *instances, *detached_instances = NULL;
	GList *l;
	char *query;
	char *iso_start, *iso_end;
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);

	/* Generate objects */
	if (uid && *uid) {
		if (!e_cal_get_objects_for_uid (ecal, uid, &objects, NULL))
			return;
	}
	else {
		iso_start = isodate_from_time_t (start);
		if (!iso_start)
			return;

		iso_end = isodate_from_time_t (end);
		if (!iso_end) {
			g_free (iso_start);
			return;
		}

		query = g_strdup_printf ("(occur-in-time-range? (make-time \"%s\") (make-time \"%s\"))",
					 iso_start, iso_end);
		g_free (iso_start);
		g_free (iso_end);
		if (!e_cal_get_object_list_as_comp (ecal, query, &objects, NULL)) {
			g_free (query);
			return;
		}
		g_free (query);
	}

	instances = NULL;

	for (l = objects; l; l = l->next) {
		ECalComponent *comp;
		icaltimezone *default_zone;

		if (priv->default_zone)
			default_zone = priv->default_zone;
		else
			default_zone = icaltimezone_get_utc_timezone ();

		comp = l->data;
		if (e_cal_component_is_instance (comp)) {
			struct comp_instance *ci;
			struct icaltimetype start_time, end_time;

			/* keep the detached instances apart */
			ci = g_new0 (struct comp_instance, 1);
			ci->comp = comp;
	
			start_time = icalcomponent_get_dtstart (e_cal_component_get_icalcomponent (comp));
			end_time = icalcomponent_get_dtend (e_cal_component_get_icalcomponent (comp));

			ci->start = icaltime_as_timet_with_zone (start_time, start_time.zone);
			ci->end = icaltime_as_timet_with_zone (end_time, end_time.zone);
			
			detached_instances = g_list_prepend (detached_instances, ci);
		} else {
			e_cal_recur_generate_instances (comp, start, end, add_instance, &instances,
							e_cal_resolve_tzid_cb, ecal,
							default_zone);

			g_object_unref (comp);
		}
	}

	g_list_free (objects);

	/* Generate instances and spew them out */

	instances = g_list_sort (instances, compare_comp_instance);
	instances = process_detached_instances (instances, detached_instances);

	for (l = instances; l; l = l->next) {
		struct comp_instance *ci;
		gboolean result;
		
		ci = l->data;
		
		result = (* cb) (ci->comp, ci->start, ci->end, cb_data);

		if (!result)
			break;
	}

	/* Clean up */

	for (l = instances; l; l = l->next) {
		struct comp_instance *ci;

		ci = l->data;
		g_object_unref (G_OBJECT (ci->comp));
		g_free (ci);
	}

	g_list_free (instances);

	for (l = detached_instances; l; l = l->next) {
		struct comp_instance *ci;

		ci = l->data;
		g_object_unref (G_OBJECT (ci->comp));
		g_free (ci);
	}

	g_list_free (detached_instances);

}

/**
 * e_cal_generate_instances:
 * @ecal: A calendar client.
 * @start: Start time for query.
 * @end: End time for query.
 * @cb: Callback for each generated instance.
 * @cb_data: Closure data for the callback.
 * 
 * Does a combination of #e_cal_get_object_list () and
 * #e_cal_recur_generate_instances().  
 *
 * The callback function should do a g_object_ref() of the calendar component
 * it gets passed if it intends to keep it around, since it will be unref'ed
 * as soon as the callback returns.
 **/
void
e_cal_generate_instances (ECal *ecal, time_t start, time_t end,
			  ECalRecurInstanceFn cb, gpointer cb_data)
{
	ECalPrivate *priv;

	g_return_if_fail (ecal != NULL);
	g_return_if_fail (E_IS_CAL (ecal));

	priv = E_CAL_GET_PRIVATE (ecal);
	g_return_if_fail (priv->load_state == E_CAL_LOAD_LOADED);

	g_return_if_fail (start >= 0);
	g_return_if_fail (end >= 0);
	g_return_if_fail (cb != NULL);

	generate_instances (ecal, start, end, NULL, cb, cb_data);
}

/**
 * e_cal_generate_instances_for_object:
 * @ecal: A calendar client.
 * @icalcomp: Object to generate instances from.
 * @start: Start time for query.
 * @end: End time for query.
 * @cb: Callback for each generated instance.
 * @cb_data: Closure data for the callback.
 *
 * Does a combination of #e_cal_get_object_list () and
 * #e_cal_recur_generate_instances(), like #e_cal_generate_instances(), but
 * for a single object.
 *
 * The callback function should do a g_object_ref() of the calendar component
 * it gets passed if it intends to keep it around, since it will be unref'ed
 * as soon as the callback returns.
 **/
void
e_cal_generate_instances_for_object (ECal *ecal, icalcomponent *icalcomp,
				     time_t start, time_t end,
				     ECalRecurInstanceFn cb, gpointer cb_data)
{
	ECalPrivate *priv;
	ECalComponent *comp;
	const char *uid, *rid;
	gboolean result;
	GList *instances = NULL;

	g_return_if_fail (E_IS_CAL (ecal));
	g_return_if_fail (start >= 0);
	g_return_if_fail (end >= 0);
	g_return_if_fail (cb != NULL);

	priv = E_CAL_GET_PRIVATE (ecal);

	comp = e_cal_component_new ();
	e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (icalcomp));

	/*If the backend stores it as individual instances and does not 
	 * have a master object - do not expand*/
	if (e_cal_get_static_capability (ecal, CAL_STATIC_CAPABILITY_RECURRENCES_NO_MASTER)) {

		/*return the same instance */
		result = (* cb)  (comp, icaltime_as_timet_with_zone (icalcomponent_get_dtstart (icalcomp), priv->default_zone),
				icaltime_as_timet_with_zone (icalcomponent_get_dtend (icalcomp), priv->default_zone), cb_data);
		g_object_unref (comp);
		return;
	}
	
	e_cal_component_get_uid (comp, &uid);
	rid = e_cal_component_get_recurid_as_string (comp);

	/* generate all instances in the given time range */
	generate_instances (ecal, start, end, uid, add_instance, &instances);

	/* now only return back the instances for the given object */
	result = TRUE;
	while (instances != NULL) {
		struct comp_instance *ci;
		const char *instance_rid;

		ci = instances->data;

		if (result) {
			instance_rid = e_cal_component_get_recurid_as_string (ci->comp);

			if (rid && *rid) {
				if (instance_rid && *instance_rid && strcmp (rid, instance_rid) == 0)
					result = (* cb) (ci->comp, ci->start, ci->end, cb_data);
			} else
				result = (* cb)  (ci->comp, ci->start, ci->end, cb_data);
		}

		/* remove instance from list */
		instances = g_list_remove (instances, ci);
		g_object_unref (ci->comp);
		g_free (ci);
	}

	/* clean up */
	g_object_unref (comp);
}

/* Builds a list of ECalComponentAlarms structures */
static GSList *
build_component_alarms_list (ECal *ecal, GList *object_list, time_t start, time_t end)
{
	GSList *comp_alarms;
	GList *l;
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);
	comp_alarms = NULL;

	for (l = object_list; l != NULL; l = l->next) {
		ECalComponent *comp;
		ECalComponentAlarms *alarms;
		ECalComponentAlarmAction omit[] = {-1};

		comp = e_cal_component_new ();
		if (!e_cal_component_set_icalcomponent (comp, icalcomponent_new_clone (l->data))) {
			g_object_unref (G_OBJECT (comp));
			continue;
		}

		alarms = e_cal_util_generate_alarms_for_comp (comp, start, end, omit, e_cal_resolve_tzid_cb,
							      ecal, priv->default_zone);
		if (alarms)
			comp_alarms = g_slist_prepend (comp_alarms, alarms);
	}

	return comp_alarms;
}

/**
 * e_cal_get_alarms_in_range:
 * @ecal: A calendar client.
 * @start: Start time for query.
 * @end: End time for query.
 *
 * Queries a calendar for the alarms that trigger in the specified range of
 * time.
 *
 * Return value: A list of #ECalComponentAlarms structures.  This should be freed
 * using the #e_cal_free_alarms() function, or by freeing each element
 * separately with #e_cal_component_alarms_free() and then freeing the list with
 * #g_slist_free().
 **/
GSList *
e_cal_get_alarms_in_range (ECal *ecal, time_t start, time_t end)
{
	ECalPrivate *priv;
	GSList *alarms;
	char *sexp, *iso_start, *iso_end;
	GList *object_list = NULL;

	g_return_val_if_fail (ecal != NULL, NULL);
	g_return_val_if_fail (E_IS_CAL (ecal), NULL);

	priv = E_CAL_GET_PRIVATE (ecal);
	g_return_val_if_fail (priv->load_state == E_CAL_LOAD_LOADED, NULL);

	g_return_val_if_fail (start >= 0 && end >= 0, NULL);
	g_return_val_if_fail (start <= end, NULL);

	iso_start = isodate_from_time_t (start);
	if (!iso_start)
		return NULL;

	iso_end = isodate_from_time_t (end);
	if (!iso_end) {
		g_free (iso_start);
		return NULL;
	}

	/* build the query string */
	sexp = g_strdup_printf ("(has-alarms-in-range? (make-time \"%s\") (make-time \"%s\"))",
				iso_start, iso_end);
	g_free (iso_start);
	g_free (iso_end);

	/* execute the query on the server */
	if (!e_cal_get_object_list (ecal, sexp, &object_list, NULL)) {
		g_free (sexp);
		return NULL;
	}

	alarms = build_component_alarms_list (ecal, object_list, start, end);

	g_list_foreach (object_list, (GFunc) icalcomponent_free, NULL);
	g_list_free (object_list);
	g_free (sexp);

	return alarms;
}

/**
 * e_cal_free_alarms:
 * @comp_alarms: A list of #ECalComponentAlarms structures.
 * 
 * Frees a list of #ECalComponentAlarms structures as returned by
 * e_cal_get_alarms_in_range().
 **/
void
e_cal_free_alarms (GSList *comp_alarms)
{
	GSList *l;

	for (l = comp_alarms; l; l = l->next) {
		ECalComponentAlarms *alarms;

		alarms = l->data;
		g_assert (alarms != NULL);

		e_cal_component_alarms_free (alarms);
	}

	g_slist_free (comp_alarms);
}

/**
 * e_cal_get_alarms_for_object:
 * @ecal: A calendar client.
 * @uid: Unique identifier for a calendar component.
 * @start: Start time for query.
 * @end: End time for query.
 * @alarms: Return value for the component's alarm instances.  Will return NULL
 * if no instances occur within the specified time range.  This should be freed
 * using the e_cal_component_alarms_free() function.
 *
 * Queries a calendar for the alarms of a particular object that trigger in the
 * specified range of time.
 *
 * Return value: TRUE on success, FALSE if the object was not found.
 **/
gboolean
e_cal_get_alarms_for_object (ECal *ecal, const char *uid,
			     time_t start, time_t end,
			     ECalComponentAlarms **alarms)
{
	ECalPrivate *priv;
	icalcomponent *icalcomp;
	ECalComponent *comp;
	ECalComponentAlarmAction omit[] = {-1};

	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	priv = E_CAL_GET_PRIVATE (ecal);
	g_return_val_if_fail (priv->load_state == E_CAL_LOAD_LOADED, FALSE);

	g_return_val_if_fail (uid != NULL, FALSE);
	g_return_val_if_fail (start >= 0 && end >= 0, FALSE);
	g_return_val_if_fail (start <= end, FALSE);
	g_return_val_if_fail (alarms != NULL, FALSE);

	*alarms = NULL;

	if (!e_cal_get_object (ecal, uid, NULL, &icalcomp, NULL))
		return FALSE;
	if (!icalcomp)
		return FALSE;

	comp = e_cal_component_new ();
	if (!e_cal_component_set_icalcomponent (comp, icalcomp)) {
		icalcomponent_free (icalcomp);
		g_object_unref (G_OBJECT (comp));
		return FALSE;
	}

	*alarms = e_cal_util_generate_alarms_for_comp (comp, start, end, omit, e_cal_resolve_tzid_cb,
						       ecal, priv->default_zone);

	return TRUE;
}

/**
 * e_cal_discard_alarm
 * @ecal: A calendar ecal.
 * @comp: The component to discard the alarm from.
 * @auid: Unique identifier of the alarm to be discarded.
 * @error: Placeholder for error information.
 *
 * Tells the calendar backend to get rid of the alarm identified by the
 * @auid argument in @comp. Some backends might remove the alarm or
 * update internal information about the alarm be discarded, or, like
 * the file backend does, ignore the operation.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_discard_alarm (ECal *ecal, ECalComponent *comp, const char *auid, GError **error)
{
	ECalPrivate *priv;
	const char *uid;

	g_return_val_if_fail (ecal != NULL, FALSE);
	g_return_val_if_fail (E_IS_CAL (ecal), FALSE);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	e_cal_component_get_uid (comp, &uid);

	if (!org_gnome_evolution_dataserver_calendar_Cal_discard_alarm (priv->proxy, uid, auid, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

typedef struct _ForeachTZIDCallbackData ForeachTZIDCallbackData;
struct _ForeachTZIDCallbackData {
	ECal *ecal;
	GHashTable *timezone_hash;
	gboolean include_all_timezones;
	gboolean success;
};

/* This adds the VTIMEZONE given by the TZID parameter to the GHashTable in
   data. */
static void
foreach_tzid_callback (icalparameter *param, void *cbdata)
{
	ForeachTZIDCallbackData *data = cbdata;
	ECalPrivate *priv;
	const char *tzid;
	icaltimezone *zone;
	icalcomponent *vtimezone_comp;
	char *vtimezone_as_string;

	priv = E_CAL_GET_PRIVATE (data->ecal);

	/* Get the TZID string from the parameter. */
	tzid = icalparameter_get_tzid (param);
	if (!tzid)
		return;

	/* Check if we've already added it to the GHashTable. */
	if (g_hash_table_lookup (data->timezone_hash, tzid))
		return;

	if (data->include_all_timezones) {
		if (!e_cal_get_timezone (data->ecal, tzid, &zone, NULL)) {
			data->success = FALSE;
			return;
		}
	} else {
		/* Check if it is in our cache. If it is, it must already be
		   on the server so return. */
		if (g_hash_table_lookup (priv->timezones, tzid))
			return;

		/* Check if it is a builtin timezone. If it isn't, return. */
		zone = icaltimezone_get_builtin_timezone_from_tzid (tzid);
		if (!zone)
			return;
	}

	/* Convert it to a string and add it to the hash. */
	vtimezone_comp = icaltimezone_get_component (zone);
	if (!vtimezone_comp)
		return;

	vtimezone_as_string = icalcomponent_as_ical_string (vtimezone_comp);

	g_hash_table_insert (data->timezone_hash, (char*) tzid,
			     g_strdup (vtimezone_as_string));
}

/* This appends the value string to the GString given in data. */
static void
append_timezone_string (gpointer key, gpointer value, gpointer data)
{
	GString *vcal_string = data;

	g_string_append (vcal_string, value);
	g_free (value);
}


/* This simply frees the hash values. */
static void
free_timezone_string (gpointer key, gpointer value, gpointer data)
{
	g_free (value);
}


/* This converts the VEVENT/VTODO to a string. If include_all_timezones is
   TRUE, it includes all the VTIMEZONE components needed for the VEVENT/VTODO.
   If not, it only includes builtin timezones that may not be on the server.

   To do that we check every TZID in the component to see if it is a builtin
   timezone. If it is, we see if it it in our cache. If it is in our cache,
   then we know the server already has it and we don't need to send it.
   If it isn't in our cache, then we need to send it to the server.
   If we need to send any timezones to the server, then we have to create a
   complete VCALENDAR object, otherwise we can just send a single VEVENT/VTODO
   as before. */
static char*
e_cal_get_component_as_string_internal (ECal *ecal,
					icalcomponent *icalcomp,
					gboolean include_all_timezones)
{
	GHashTable *timezone_hash;
	GString *vcal_string;
	int initial_vcal_string_len;
	ForeachTZIDCallbackData cbdata;
	char *obj_string;
	ECalPrivate *priv;

	priv = E_CAL_GET_PRIVATE (ecal);

	timezone_hash = g_hash_table_new (g_str_hash, g_str_equal);

	/* Add any timezones needed to the hash. We use a hash since we only
	   want to add each timezone once at most. */
	cbdata.ecal = ecal;
	cbdata.timezone_hash = timezone_hash;
	cbdata.include_all_timezones = include_all_timezones;
	cbdata.success = TRUE;
	icalcomponent_foreach_tzid (icalcomp, foreach_tzid_callback, &cbdata);
	if (!cbdata.success) {
		g_hash_table_foreach (timezone_hash, free_timezone_string,
				      NULL);
		return NULL;
	}

	/* Create the start of a VCALENDAR, to add the VTIMEZONES to,
	   and remember its length so we know if any VTIMEZONEs get added. */
	vcal_string = g_string_new (NULL);
	g_string_append (vcal_string,
			 "BEGIN:VCALENDAR\n"
			 "PRODID:-//Ximian//NONSGML Evolution Calendar//EN\n"
			 "VERSION:2.0\n"
			 "METHOD:PUBLISH\n");
	initial_vcal_string_len = vcal_string->len;

	/* Now concatenate all the timezone strings. This also frees the
	   timezone strings as it goes. */
	g_hash_table_foreach (timezone_hash, append_timezone_string,
			      vcal_string);

	/* Get the string for the VEVENT/VTODO. */
	obj_string = g_strdup (icalcomponent_as_ical_string (icalcomp));

	/* If there were any timezones to send, create a complete VCALENDAR,
	   else just send the VEVENT/VTODO string. */
	if (!include_all_timezones
	    && vcal_string->len == initial_vcal_string_len) {
		g_string_free (vcal_string, TRUE);
	} else {
		g_string_append (vcal_string, obj_string);
		g_string_append (vcal_string, "END:VCALENDAR\n");
		g_free (obj_string);
		obj_string = vcal_string->str;
		g_string_free (vcal_string, FALSE);
	}

	g_hash_table_destroy (timezone_hash);

	return obj_string;
}

/**
 * e_cal_get_component_as_string:
 * @ecal: A calendar client.
 * @icalcomp: A calendar component object.
 *
 * Gets a calendar component as an iCalendar string, with a toplevel
 * VCALENDAR component and all VTIMEZONEs needed for the component.
 *
 * Return value: the component as a complete iCalendar string, or NULL on
 * failure. The string should be freed after use.
 **/
char*
e_cal_get_component_as_string (ECal *ecal, icalcomponent *icalcomp)
{
	return e_cal_get_component_as_string_internal (ecal, icalcomp, TRUE);
}

/**
 * e_cal_create_object:
 * @ecal: A calendar client.
 * @icalcomp: The component to create.
 * @uid: Return value for the UID assigned to the new component by the calendar backend.
 * @error: Placeholder for error information.
 *
 * Requests the calendar backend to create the object specified by the @icalcomp
 * argument. Some backends would assign a specific UID to the newly created object,
 * in those cases that UID would be returned in the @uid argument.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_create_object (ECal *ecal, icalcomponent *icalcomp, char **uid, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_create_object (priv->proxy, icalcomponent_as_ical_string (icalcomp), uid, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	if (!uid)
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OTHER_ERROR, error);
	else {
		icalcomponent_set_uid (icalcomp, *uid);
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
	}
}

/**
 * e_cal_modify_object:
 * @ecal: A calendar client.
 * @icalcomp: Component to modify.
 * @mod: Type of modification.
 * @error: Placeholder for error information.
 *
 * Requests the calendar backend to modify an existing object. If the object
 * does not exist on the calendar, an error will be returned.
 *
 * For recurrent appointments, the @mod argument specifies what to modify,
 * if all instances (CALOBJ_MOD_ALL), a single instance (CALOBJ_MOD_THIS),
 * or a specific set of instances (CALOBJ_MOD_THISNADPRIOR and
 * CALOBJ_MOD_THISANDFUTURE).
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_modify_object (ECal *ecal, icalcomponent *icalcomp, CalObjModType mod, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (icalcomp, E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_modify_object (priv->proxy, icalcomponent_as_ical_string (icalcomp), mod, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

/**
 * e_cal_remove_object_with_mod:
 * @ecal: A calendar client.
 * @uid: UID og the object to remove.
 * @rid: Recurrence ID of the specific recurrence to remove.
 * @mod: Type of removal.
 * @error: Placeholder for error information.
 *
 * This function allows the removal of instances of a recurrent
 * appointment. By using a combination of the @uid, @rid and @mod
 * arguments, you can remove specific instances. If what you want
 * is to remove all instances, use e_cal_remove_object instead.
 *
 * If not all instances are removed, the client will get a "obj_modified"
 * signal, while it will get a "obj_removed" signal when all instances
 * are removed.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_remove_object_with_mod (ECal *ecal, const char *uid,
			      const char *rid, CalObjModType mod, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (uid, E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_remove_object (priv->proxy, uid, rid ? rid : "", mod, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

/**
 * e_cal_remove_object:
 * @ecal:  A calendar client.
 * @uid: Unique identifier of the calendar component to remove.
 * @error: Placeholder for error information.
 * 
 * Asks a calendar to remove a component.  If the server is able to remove the
 * component, all clients will be notified and they will emit the "obj_removed"
 * signal.
 * 
 * Return value: %TRUE if successful, %FALSE otherwise.
 **/
gboolean
e_cal_remove_object (ECal *ecal, const char *uid, GError **error)
{
	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (uid, E_CALENDAR_STATUS_INVALID_ARG);

	return e_cal_remove_object_with_mod (ecal, uid, NULL, CALOBJ_MOD_THIS, error);
}

/**
 * e_cal_receive_objects:
 * @ecal:  A calendar client.
 * @icalcomp: An icalcomponent.
 * @error: Placeholder for error information.
 *
 * Makes the backend receive the set of iCalendar objects specified in the
 * @icalcomp argument. This is used for iTIP confirmation/cancellation
 * messages for scheduled meetings.
 *
 * Return value: %TRUE if successful, %FALSE otherwise.
 */
gboolean
e_cal_receive_objects (ECal *ecal, icalcomponent *icalcomp, GError **error)
{
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_receive_objects (priv->proxy, icalcomponent_as_ical_string (icalcomp), error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

/**
 * e_cal_send_objects:
 * @ecal: A calendar client.
 * @icalcomp: An icalcomponent.
 * @users: List of users to send the objects to.
 * @modified_icalcomp: Return value for the icalcomponent after all the operations
 * performed.
 * @error: Placeholder for error information.
 *
 * Requests a calendar backend to send meeting information to the specified list
 * of users.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_send_objects (ECal *ecal, icalcomponent *icalcomp, GList **users, icalcomponent **modified_icalcomp, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	char **users_array;
	char *object;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	if (!org_gnome_evolution_dataserver_calendar_Cal_send_objects (priv->proxy, icalcomponent_as_ical_string (icalcomp), &users_array, &object, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);		
	}

	status = E_CALENDAR_STATUS_OK;
	*users = NULL;
	if (users_array) {
		char **user;
		*modified_icalcomp = icalparser_parse_string (object);
		if (!(*modified_icalcomp))
			status = E_CALENDAR_STATUS_INVALID_OBJECT;
			
		for (user = users_array; *user; user++)
			*users = g_list_append (*users, g_strdup (*user));
		g_strfreev (users_array);
	} else
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OTHER_ERROR, error);

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_get_timezone:
 * @ecal: A calendar client.
 * @tzid: ID of the timezone to retrieve.
 * @zone: Return value for the timezone.
 * @error: Placeholder for error information.
 *
 * Retrieves a timezone object from the calendar backend.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_get_timezone (ECal *ecal, const char *tzid, icaltimezone **zone, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	icalcomponent *icalcomp;
	gchar *object;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (zone, E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	/* Check for well known zones and in the cache */
	*zone = NULL;
	
	/* If tzid is NULL or "" we return NULL, since it is a 'local time'. */
	if (!tzid || !tzid[0]) {
		*zone = NULL;		
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);		
	}

	/* If it is UTC, we return the special UTC timezone. */
	if (!strcmp (tzid, "UTC")) {
		*zone = icaltimezone_get_utc_timezone ();
	} else {
		/* See if we already have it in the cache. */
		*zone = g_hash_table_lookup (priv->timezones, tzid);
	}
	
	if (*zone) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);	
	}
	
	/* call the backend */
	if (!org_gnome_evolution_dataserver_calendar_Cal_get_timezone (priv->proxy, tzid, &object, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	status = E_CALENDAR_STATUS_OK;
	icalcomp = NULL;
	if (object) {
		icalcomp = icalparser_parse_string (object);
		if (!icalcomp)
			status = E_CALENDAR_STATUS_INVALID_OBJECT;
		g_free (object);
	}
	
	if (!icalcomp) {
		E_CALENDAR_CHECK_STATUS (status, error);
	}
	
	*zone = icaltimezone_new ();	
	if (!icaltimezone_set_component (*zone, icalcomp)) {
		icaltimezone_free (*zone, 1);
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OBJECT_NOT_FOUND, error);
	}

	/* Now add it to the cache, to avoid the server call in future. */
	g_hash_table_insert (priv->timezones, icaltimezone_get_tzid (*zone), *zone);

	E_CALENDAR_CHECK_STATUS (status, error);
}

/**
 * e_cal_add_timezone
 * @ecal: A calendar client.
 * @izone: The timezone to add.
 * @error: Placeholder for error information.
 *
 * Add a VTIMEZONE object to the given calendar.
 *
 * Returns: TRUE if successful, FALSE otherwise.
 */
gboolean
e_cal_add_timezone (ECal *ecal, icaltimezone *izone, GError **error)
{
	ECalPrivate *priv;
	const char *tzobj;
	icalcomponent *icalcomp;
	gchar *tzid;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (izone, E_CALENDAR_STATUS_INVALID_ARG);
	
	priv = E_CAL_GET_PRIVATE (ecal);

	/* TODO: mutex */
/*	g_mutex_lock (priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	/* Make sure we have a valid component - UTC doesn't, nor do
	 * we really have to add it */
	if (izone == icaltimezone_get_utc_timezone ()) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
	}
	
	icalcomp = icaltimezone_get_component (izone);
	if (!icalcomp) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_INVALID_ARG, error);
	}

	/* convert icaltimezone into a string */	
	tzobj = icalcomponent_as_ical_string (icalcomp);

	/* call the backend */
	/* FIXME: tzid doesn't get used? */
	if (!org_gnome_evolution_dataserver_calendar_Cal_add_timezone (priv->proxy, tzobj, &tzid, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

static char*
construct_calviewlistener_path (void)
{
	static guint counter = 1;
	return g_strdup_printf ("/org/gnome/evolution/dataserver/calendar/DataCalViewListener/%d/%d", getpid(), counter++);
}

/**
 * e_cal_get_query:
 * @ecal: A calendar client.
 * @sexp: S-expression representing the query.
 * @query: Return value for the new query.
 * @error: Placeholder for error information.
 * 
 * Creates a live query object from a loaded calendar.
 * 
 * Return value: A query object that will emit notification signals as calendar
 * components are added and removed from the query in the server.
 **/
gboolean
e_cal_get_query (ECal *ecal, const char *sexp, ECalView **query, GError **error)
{
	ECalPrivate *priv;
	ECalendarStatus status;
	gchar *listener_path, *query_path;
	const char *address;
	ECalViewListener *listener;
	DBusGProxy *query_proxy;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);
	e_return_error_if_fail (query, E_CALENDAR_STATUS_INVALID_ARG);

	priv = E_CAL_GET_PRIVATE (ecal);
	
	/* TODO: mutex */
/*	g_mutex_lock (ecal->priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	listener = e_cal_view_listener_new ();
	listener_path = construct_calviewlistener_path ();
	dbus_g_connection_register_g_object (connection, listener_path, G_OBJECT(listener));
	address = dbus_bus_get_unique_name (dbus_g_connection_get_connection (connection));
	
	if (!org_gnome_evolution_dataserver_calendar_Cal_get_query (priv->proxy, sexp, address, listener_path, &query_path, error)) {
		g_free (listener_path);
		g_object_unref (listener);
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	status = E_CALENDAR_STATUS_OK;
/*	g_warning ("%s: %s", G_STRLOC, query_path);*/
	query_proxy = dbus_g_proxy_new_for_name_owner (connection,
                                                "org.gnome.evolution.dataserver.Calendar", query_path,
                                                "org.gnome.evolution.dataserver.calendar.CalView", error);
																
	if (!query_proxy) {
		*query = NULL;
		status = E_CALENDAR_STATUS_OTHER_ERROR;
	} else {
		*query = e_cal_view_new (query_proxy, listener, ecal);
	}

	g_object_unref (query_proxy);
	g_object_unref (listener);
	g_free (listener_path);

	E_CALENDAR_CHECK_STATUS (status, error);
}


/* This ensures that the given timezone is on the server. We use this to pass
   the default timezone to the server, so it can resolve DATE and floating
   DATE-TIME values into specific times. (Most of our IDL interface uses
   time_t values to pass specific times from the server to the ecal.) */
static gboolean
e_cal_ensure_timezone_on_server (ECal *ecal, icaltimezone *zone, GError **error)
{
	ECalPrivate *priv;
	char *tzid;
	icaltimezone *tmp_zone;

	priv = E_CAL_GET_PRIVATE (ecal);

	/* FIXME This is highly broken since there is no locking */

	/* If the zone is NULL or UTC we don't need to do anything. */
	if (!zone)
		return TRUE;
	
	tzid = icaltimezone_get_tzid (zone);

	if (!strcmp (tzid, "UTC"))
		return TRUE;

	/* See if we already have it in the cache. If we do, it must be on
	   the server already. */
	tmp_zone = g_hash_table_lookup (priv->timezones, tzid);
	if (tmp_zone)
		return TRUE;

	/* Now we have to send it to the server, in case it doesn't already
	   have it. */
	return e_cal_add_timezone (ecal, zone, error);
}

/**
 * e_cal_set_default_timezone:
 * @ecal: A calendar client.
 * @zone: A timezone object.
 * @error: Placeholder for error information.
 *
 * Sets the default timezone on the calendar.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_set_default_timezone (ECal *ecal, icaltimezone *zone, GError **error)
{
	ECalPrivate *priv;
	const char *tzid;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	
	e_return_error_if_fail (zone, E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);

	/* Don't set the same timezone multiple times */
	if (priv->default_zone == zone)
		return FALSE;
	
	/* Make sure the server has the VTIMEZONE data. */
	if (!e_cal_ensure_timezone_on_server (ecal, zone, error))
		return FALSE;

	/* TODO: mutex */
/*	g_mutex_lock (priv->mutex);*/

	if (priv->load_state != E_CAL_LOAD_LOADED) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_URI_NOT_LOADED, error);
	}

	/* FIXME Adding it to the server to change the tzid */
	tzid = icaltimezone_get_tzid (zone);

	/* call the backend */
	if (!org_gnome_evolution_dataserver_calendar_Cal_set_default_timezone (priv->proxy, tzid, error)) {
		E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_CORBA_EXCEPTION, error);
	}

	E_CALENDAR_CHECK_STATUS (E_CALENDAR_STATUS_OK, error);
}

/**
 * e_cal_get_error_message
 * @status: A status code.
 *
 * Gets an error message for the given status code.
 *
 * Returns: the error message.
 */
const char *
e_cal_get_error_message (ECalendarStatus status)
{
	switch (status) {
	case E_CALENDAR_STATUS_INVALID_ARG :
		return _("Invalid argument");
	case E_CALENDAR_STATUS_BUSY :
		return _("Backend is busy");
	case E_CALENDAR_STATUS_REPOSITORY_OFFLINE :
		return _("Repository is offline");
	case E_CALENDAR_STATUS_NO_SUCH_CALENDAR :
		return _("No such calendar");
	case E_CALENDAR_STATUS_OBJECT_NOT_FOUND :
		return _("Object not found");
	case E_CALENDAR_STATUS_INVALID_OBJECT :
		return _("Invalid object");
	case E_CALENDAR_STATUS_URI_NOT_LOADED :
		return _("URI not loaded");
	case E_CALENDAR_STATUS_URI_ALREADY_LOADED :
		return _("URI already loaded");
	case E_CALENDAR_STATUS_PERMISSION_DENIED :
		return _("Permission denied");
	case E_CALENDAR_STATUS_UNKNOWN_USER :
		return _("Unknown User");
	case E_CALENDAR_STATUS_OBJECT_ID_ALREADY_EXISTS :
		return _("Object ID already exists");
	case E_CALENDAR_STATUS_PROTOCOL_NOT_SUPPORTED :
		return _("Protocol not supported");
	case E_CALENDAR_STATUS_CANCELLED :
		return _("Operation has been cancelled");
	case E_CALENDAR_STATUS_COULD_NOT_CANCEL :
		return _("Could not cancel operation");
	case E_CALENDAR_STATUS_AUTHENTICATION_FAILED :
		return _("Authentication failed");
	case E_CALENDAR_STATUS_AUTHENTICATION_REQUIRED :
		return _("Authentication required");
	case E_CALENDAR_STATUS_CORBA_EXCEPTION :
		return _("A CORBA exception has occurred");
	case E_CALENDAR_STATUS_OTHER_ERROR :
		return _("Unknown error");
	case E_CALENDAR_STATUS_OK :
		return _("No error");
	default:
		/* ignore everything else */
		break;
	}

	return NULL;
}

static gboolean
get_default (ECal **ecal, ESourceList *sources, ECalSourceType type, ECalAuthFunc func, gpointer data, GError **error)
{
	GSList *g;
	GError *err = NULL;
	ESource *default_source = NULL;
	gboolean rv = TRUE;

	for (g = e_source_list_peek_groups (sources); g; g = g->next) {
		ESourceGroup *group = E_SOURCE_GROUP (g->data);
		GSList *s;
		for (s = e_source_group_peek_sources (group); s; s = s->next) {
			ESource *source = E_SOURCE (s->data);

			if (e_source_get_property (source, "default")) {
				default_source = source;
				break;
			}
		}

		if (default_source)
			break;
	}

	if (default_source) {
		*ecal = e_cal_new (default_source, type);
		if (!*ecal) {			
			g_propagate_error (error, err);
			rv = FALSE;
			goto done;
		}

		e_cal_set_auth_func (*ecal, func, data);
		if (!e_cal_open (*ecal, TRUE, &err)) {
			g_propagate_error (error, err);
			rv = FALSE;
			goto done;		
		}
	} else {
		switch (type) {
		case E_CAL_SOURCE_TYPE_EVENT:
			*ecal = e_cal_new_system_calendar ();
			break;
		case E_CAL_SOURCE_TYPE_TODO:
			*ecal = e_cal_new_system_tasks ();
			break;
		default:
			break;
		}
		
		if (!*ecal) {			
			g_propagate_error (error, err);
			rv = FALSE;
			goto done;
		}

		e_cal_set_auth_func (*ecal, func, data);
		if (!e_cal_open (*ecal, TRUE, &err)) {
			g_propagate_error (error, err);
			rv = FALSE;
			goto done;		
		}
	}

 done:
	if (!rv && *ecal) {
		g_object_unref (*ecal);
		*ecal = NULL;
	}
	g_object_unref (sources);

	return rv;
}

/**
 * e_cal_open_default:
 * @ecal: A calendar client.
 * @type: Type of the calendar.
 * @func: Authentication function.
 * @data: Closure data for the authentication function.
 * @error: Placeholder for error information.
 *
 * Opens the default calendar.
 *
 * Return value: TRUE if it opened correctly, FALSE otherwise.
 */
gboolean
e_cal_open_default (ECal **ecal, ECalSourceType type, ECalAuthFunc func, gpointer data, GError **error)
{
	ESourceList *sources;
	GError *err = NULL;

	if (!e_cal_get_sources (&sources, type, &err)) {
		g_propagate_error (error, err);
		return FALSE;
	}

	return get_default (ecal, sources, type, func, data, error);
}

/**
 * e_cal_set_default:
 * @ecal: A calendar client.
 * @error: Placeholder for error information.
 *
 * Sets a calendar as the default one.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_set_default (ECal *ecal, GError **error)
{
	ESource *source;
	ECalPrivate *priv;

	e_return_error_if_fail (ecal && E_IS_CAL (ecal), E_CALENDAR_STATUS_INVALID_ARG);	

	priv = E_CAL_GET_PRIVATE (ecal);
	source = e_cal_get_source (ecal);
	if (!source) {
		/* XXX gerror */
		return FALSE;
	}

	return e_cal_set_default_source (source, priv->type, error);
}

static gboolean
set_default_source (ESourceList *sources, ESource *source, GError **error)
{
	const char *uid;
	GError *err = NULL;
	GSList *g;

	uid = e_source_peek_uid (source);

	/* make sure the source is actually in the ESourceList.  if
	   it's not we don't bother adding it, just return an error */
	source = e_source_list_peek_source_by_uid (sources, uid);
	if (!source) {
		/* XXX gerror */
		g_object_unref (sources);
		return FALSE;
	}

	/* loop over all the sources clearing out any "default"
	   properties we find */
	for (g = e_source_list_peek_groups (sources); g; g = g->next) {
		GSList *s;
		for (s = e_source_group_peek_sources (E_SOURCE_GROUP (g->data));
		     s; s = s->next) {
			e_source_set_property (E_SOURCE (s->data), "default", NULL);
		}
	}

	/* set the "default" property on the source */
	e_source_set_property (source, "default", "true");

	if (!e_source_list_sync (sources, &err)) {
		g_propagate_error (error, err);
		return FALSE;
	}

	return TRUE;
}

/**
 * e_cal_set_default_source:
 * @source: An #ESource.
 * type: Type of the source.
 * @error: Placeholder for error information.
 *
 * Sets the default source for the specified @type.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_set_default_source (ESource *source, ECalSourceType type, GError **error)
{
	ESourceList *sources;
	GError *err = NULL;

	if (!e_cal_get_sources (&sources, type, &err)) {
		g_propagate_error (error, err);
		return FALSE;
	}

	return set_default_source (sources, source, error);
}

static gboolean
get_sources (ESourceList **sources, const char *key, GError **error)
{
	GConfClient *gconf = gconf_client_get_default();

	*sources = e_source_list_new_for_gconf (gconf, key);
	g_object_unref (gconf);

	return TRUE;
}

/**
 * e_cal_get_sources:
 * @sources: Return value for list of sources.
 * @type: Type of the sources to get.
 * @error: Placeholder for error information.
 *
 * Gets the list of sources defined in the configuration for the given @type.
 *
 * Return value: TRUE if the operation was successful, FALSE otherwise.
 */
gboolean
e_cal_get_sources (ESourceList **sources, ECalSourceType type, GError **error)
{
	switch (type) {
	case E_CAL_SOURCE_TYPE_EVENT:
		return get_sources (sources, "/apps/evolution/calendar/sources", error);		
		break;
	case E_CAL_SOURCE_TYPE_TODO:
		return get_sources (sources, "/apps/evolution/tasks/sources", error);
		break;
	default:
		/* FIXME Fill in error */
		return FALSE;
	}
	
	/* FIXME Fill in error */
	return FALSE;	
}
