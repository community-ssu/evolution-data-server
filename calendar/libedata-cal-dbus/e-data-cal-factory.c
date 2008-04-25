/* Evolution calendar factory
 *
 * Copyright (C) 2000-2003 Ximian, Inc.
 *
 * Authors: 
 *   Federico Mena-Quintero <federico@ximian.com>
 *   JP Rosevear <jpr@ximian.com>
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

#include <stdlib.h>
#include <string.h>
 
/* DBus includes */
#include <glib-object.h>
#include <dbus/dbus-protocol.h>
#include <dbus/dbus-glib.h>
#include <dbus/dbus-glib-bindings.h>

#include <libedataserver/e-url.h>
#include <libedataserver/e-source.h>
#include <libedataserver/e-data-server-module.h>
#include "e-cal-backend.h"
#include "e-cal-backend-factory.h"
#include "e-data-cal.h"
#include "e-data-cal-factory.h"

static gboolean impl_CalFactory_getCal (EDataCalFactory *factory, const char *IN_uri, EDataCalObjType type, char **OUT_calendar, GError **error);
#include "e-data-cal-factory-glue.h"

static EDataCalFactory *factory;
DBusGConnection *connection;

/* Convenience macro to test and set a GError/return on failure */
#define g_set_error_val_if_fail(test, returnval, error, domain, code) G_STMT_START{ \
 if G_LIKELY (test) {} else { \
   g_set_error (error, domain, code, #test); \
   g_warning(#test " failed"); \
   return (returnval); \
 } \
}G_STMT_END

/* Generate the GObject boilerplate */
G_DEFINE_TYPE(EDataCalFactory, e_data_cal_factory, G_TYPE_OBJECT);

#define E_DATA_CAL_FACTORY_GET_PRIVATE(o) (G_TYPE_INSTANCE_GET_PRIVATE ((o), E_TYPE_DATA_CAL_FACTORY, EDataCalFactoryPrivate))

struct _EDataCalFactoryPrivate {
	GMutex *backend_lock;
	GHashTable *backends;
	
	GMutex *calendars_lock;
	GHashTable *calendars;
	
	int mode;
	GHashTable *methods;
};

void e_data_cal_factory_register_backends (EDataCalFactory *cal_factory);

/* Class init */
static void
e_data_cal_factory_class_init (EDataCalFactoryClass *e_data_cal_factory_class)
{
  g_type_class_add_private (e_data_cal_factory_class, sizeof (EDataCalFactoryPrivate));
  dbus_g_object_type_install_info (G_TYPE_FROM_CLASS (e_data_cal_factory_class), &dbus_glib_e_data_cal_factory_object_info);
}

/* Instance init */
static void
e_data_cal_factory_init (EDataCalFactory *factory)
{
  EDataCalFactoryPrivate *priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);
  
  priv->methods = g_hash_table_new_full (g_str_hash, g_str_equal, 
						(GDestroyNotify) g_free, (GDestroyNotify) g_hash_table_destroy);
  
  priv->backend_lock = g_mutex_new ();
  priv->backends = g_hash_table_new (g_str_hash, g_str_equal);

  priv->calendars_lock = g_mutex_new ();
  priv->calendars = g_hash_table_new_full (g_str_hash, g_str_equal, g_free, NULL);
  
  e_data_server_module_init ();
  e_data_cal_factory_register_backends (factory);
}

/* Create the EDataCalFactory error quark */
GQuark
e_data_cal_factory_error_quark (void)
{
  static GQuark quark = 0;
  if (!quark)
    quark = g_quark_from_static_string ("e_data_cal_factory_error");
  return quark;
}

/* Opening calendars */

static icalcomponent_kind
calobjtype_to_icalkind (const EDataCalObjType type)
{
	switch (type){
	case Event:
		return ICAL_VEVENT_COMPONENT;
	case Todo:
		return ICAL_VTODO_COMPONENT;
	case Journal:
		return ICAL_VJOURNAL_COMPONENT;
	}
	
	return ICAL_NO_COMPONENT;
}

static ECalBackendFactory*
get_backend_factory (GHashTable *methods, const char *method, icalcomponent_kind kind)
{
	GHashTable *kinds;
	ECalBackendFactory *factory;
	
	kinds = g_hash_table_lookup (methods, method);
	if (!kinds)
		return 0;

	factory = g_hash_table_lookup (kinds, GINT_TO_POINTER (kind));

	if (!factory)
		g_printerr ("No factory found\n");
	return factory;
}

/* Looks up a calendar backend in a factory's hash table of uri->cal.  If
 * *non-NULL, orig_uri_return will be set to point to the original key in the
 * *hash table.
 */
static ECalBackend *
lookup_backend (EDataCalFactory *factory, const char *uristr)
{
	EDataCalFactoryPrivate *priv;
	EUri *uri;
	ECalBackend *backend;
	char *tmp;

	priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);

	uri = e_uri_new (uristr);
	if (!uri)
		return NULL;

	tmp = e_uri_to_string (uri, FALSE);
	backend = g_hash_table_lookup (priv->backends, tmp);
	g_free (tmp);
	e_uri_free (uri);

	return backend;
}

static gchar* make_path_name(const gchar* uri)
{
	gchar *s, *path;
	int i;
	s = g_strdup (uri);
	for (i = 0; s[i] != '\0'; i++) {
		if (!(((s[i] >= 'a')&&(s[i] <= 'z'))||((s[i] >= 'A')&&(s[i] <= 'Z'))||((s[i] >= '0')&&(s[i] <= '9'))))
			s[i] = '_';
	}
	path = g_strdup_printf("/org/gnome/evolution/dataserver/calendar/%s", s);
	g_free (s);
	return path;
}

static void my_remove (char *key, GObject *dead)
{
  EDataCalFactoryPrivate *priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);  
  g_print ("Caught a dying calendar with UID %s\n", key);
  g_mutex_lock (priv->calendars_lock);
  g_hash_table_remove (priv->calendars, key);
  g_mutex_unlock (priv->calendars_lock);
  g_free (key);
}

/* TODO: Error checking! */
static gboolean
impl_CalFactory_getCal (EDataCalFactory *factory, const char *IN_uri, EDataCalObjType type, char **OUT_calendar, GError **error)
{
	EDataCal *calendar;
	EDataCalFactoryPrivate *priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);
	ESource *source;
	EUri *uri;
	gchar *path;
	
	g_set_error_val_if_fail (IN_uri != NULL, FALSE, error, E_DATA_CAL_FACTORY_ERROR, E_DATA_CAL_FACTORY_ERROR_GENERIC);
	g_set_error_val_if_fail (OUT_calendar != NULL, FALSE, error, E_DATA_CAL_FACTORY_ERROR, E_DATA_CAL_FACTORY_ERROR_GENERIC);

	g_mutex_lock (priv->calendars_lock);
	
	source = e_source_new_with_absolute_uri ("", IN_uri);
	path = make_path_name (IN_uri);
	calendar = g_hash_table_lookup (priv->calendars, path);
	if (calendar == NULL) {
		ECalBackend *backend = NULL;
		
		uri = e_uri_new (IN_uri);
		g_set_error_val_if_fail (uri != NULL, FALSE, error, E_DATA_CAL_FACTORY_ERROR, E_DATA_CAL_FACTORY_ERROR_GENERIC);
		
		backend = e_cal_backend_factory_new_backend (get_backend_factory (priv->methods, uri->protocol, calobjtype_to_icalkind (type)), source);
		calendar = e_data_cal_new (backend, source);
		
		e_cal_backend_set_mode (backend, priv->mode);

		g_hash_table_insert (priv->calendars, g_strdup(path), calendar);
		dbus_g_connection_register_g_object (connection, path, G_OBJECT (calendar));
		g_object_weak_ref (G_OBJECT (calendar), (GWeakNotify)my_remove, g_strdup (path));
	} else {
		g_object_ref (calendar);
	}
	*OUT_calendar = path;
	
	g_mutex_unlock (priv->calendars_lock);
	return TRUE;
}

static void 
set_backend_online_status (gpointer key, gpointer value, gpointer data)
{
	ECalBackend *backend = E_CAL_BACKEND (value);

	e_cal_backend_set_mode (backend,  GPOINTER_TO_INT (data));
}

/**
 * e_data_cal_factory_set_backend_mode:
 * @factory: A calendar factory.
 * @mode: Online mode to set.
 *
 * Sets the online mode for all backends created by the given factory.
 */
void 
e_data_cal_factory_set_backend_mode (EDataCalFactory *factory, int mode)
{
	EDataCalFactoryPrivate *priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);
	
	
	priv->mode = mode;
	g_hash_table_foreach (priv->backends, set_backend_online_status, GINT_TO_POINTER (priv->mode));
}

/**
 * e_data_cal_factory_register_backend:
 * @factory: A calendar factory.
 * @backend_factory: The object responsible for creating backends.
 * 
 * Registers an #ECalBackend subclass that will be used to handle URIs
 * with a particular method.  When the factory is asked to open a
 * particular URI, it will look in its list of registered methods and
 * create a backend of the appropriate type.
 **/
void
e_data_cal_factory_register_backend (EDataCalFactory *factory, ECalBackendFactory *backend_factory)
{
	EDataCalFactoryPrivate *priv;
	const char *method;
	char *method_str;
	GHashTable *kinds;
	GType type;
	icalcomponent_kind kind;

	g_return_if_fail (factory && E_IS_DATA_CAL_FACTORY (factory));
	g_return_if_fail (backend_factory && E_IS_CAL_BACKEND_FACTORY (backend_factory));

	priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);

	method = E_CAL_BACKEND_FACTORY_GET_CLASS (backend_factory)->get_protocol (backend_factory);
	kind = E_CAL_BACKEND_FACTORY_GET_CLASS (backend_factory)->get_kind (backend_factory);

	method_str = g_ascii_strdown (method, -1);

	kinds = g_hash_table_lookup (priv->methods, method_str);
	if (kinds) {
		type = GPOINTER_TO_INT (g_hash_table_lookup (kinds, GINT_TO_POINTER (kind)));
		if (type) {
			g_warning (G_STRLOC ": method `%s' already registered", method_str);
			g_free (method_str);

			return;
		}

		g_free (method_str);
	} else {
		kinds = g_hash_table_new_full (g_direct_hash, g_direct_equal, NULL, NULL);
		g_hash_table_insert (priv->methods, method_str, kinds);
	}
	
	g_hash_table_insert (kinds, GINT_TO_POINTER (kind), backend_factory);
}

/**
 * e_data_cal_factory_register_backends:
 * @cal_factory: A calendar factory.
 *
 * Register all backends for the given factory.
 */
void
e_data_cal_factory_register_backends (EDataCalFactory *cal_factory)
{
	GList *factories, *f;

	factories = e_data_server_get_extensions_for_type (E_TYPE_CAL_BACKEND_FACTORY);
	for (f = factories; f; f = f->next) {
		ECalBackendFactory *backend_factory = f->data;

		e_data_cal_factory_register_backend (cal_factory, g_object_ref (backend_factory));
	}

	e_data_server_extension_list_free (factories);
}

/**
 * e_data_cal_factory_get_n_backends
 * @factory: A calendar factory.
 *
 * Get the number of backends currently active in the given factory.
 *
 * Returns: the number of backends.
 */
int
e_data_cal_factory_get_n_backends (EDataCalFactory *factory)
{
	EDataCalFactoryPrivate *priv;

	g_return_val_if_fail (E_IS_DATA_CAL_FACTORY (factory), 0);

	priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);
	return g_hash_table_size (priv->backends);
}

/* Frees a uri/backend pair from the backends hash table */
static void
dump_backend (gpointer key, gpointer value, gpointer data)
{
	char *uri;
	ECalBackend *backend;

	uri = key;
	backend = value;

	g_message ("  %s: %p", uri, backend);
}

/**
 * e_data_cal_factory_dump_active_backends:
 * @factory: A calendar factory.
 *
 * Dumps to standard output a list of all active backends for the given
 * factory.
 */
void
e_data_cal_factory_dump_active_backends (EDataCalFactory *factory)
{
	EDataCalFactoryPrivate *priv;

	g_message ("Active PCS backends");

	priv = E_DATA_CAL_FACTORY_GET_PRIVATE (factory);
	g_hash_table_foreach (priv->backends, dump_backend, NULL);
}

/* Convenience function to print an error and exit */
static void
die (const char *prefix, GError *error) 
{
  g_error("%s: %s", prefix, error->message);
  g_error_free (error);
  exit(1);
}

#define E_DATA_CAL_FACTORY_SERVICE_NAME "org.gnome.evolution.dataserver.Calendar"

int
main (int argc, char **argv)
{
  GMainLoop *loop;
  GError *error = NULL;
  DBusGProxy *driver_proxy;
  guint32 request_name_ret;

  g_type_init ();
  g_thread_init (NULL);
  dbus_g_thread_init ();

  loop = g_main_loop_new (NULL, FALSE);

  g_printerr ("Launching EDataCalFactory\n");  

  /* Obtain a connection to the session bus */
  connection = dbus_g_bus_get (DBUS_BUS_SESSION, &error);
  if (connection == NULL)
    die ("Failed to open connection to bus", error);

  factory = g_object_new (E_TYPE_DATA_CAL_FACTORY, NULL);
  dbus_g_connection_register_g_object (connection,
                                       "/org/gnome/evolution/dataserver/calendar/CalFactory",
                                       G_OBJECT (factory));

  driver_proxy = dbus_g_proxy_new_for_name (connection,
                                            DBUS_SERVICE_DBUS,
                                            DBUS_PATH_DBUS,
                                            DBUS_INTERFACE_DBUS);

  if (!org_freedesktop_DBus_request_name (driver_proxy, E_DATA_CAL_FACTORY_SERVICE_NAME,
					  0, &request_name_ret, &error))
    die ("Failed to get name", error);

  if (request_name_ret != DBUS_REQUEST_NAME_REPLY_PRIMARY_OWNER) {
    g_error ("Got result code %u from requesting name", request_name_ret);
    exit (1);
  }

  g_print ("Calendar factory has name '%s'\n", E_DATA_CAL_FACTORY_SERVICE_NAME);

  g_print ("Factory entering main loop\n");
  g_main_loop_run (loop);
  g_print ("Exiting\n");

  dbus_g_connection_unref (connection);

  return 0;
}
