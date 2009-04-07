/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Logging functions for ebook
 *
 * Authors:
 *   Marco Barisione <marco@barisione.org>
 *
 * Copyright (C) 2009 Nokia Corporation
 */

#include <string.h>
 
#include "e-log.h"

static GLogLevelFlags current_log_level = G_LOG_LEVEL_MESSAGE;
static GPtrArray *enabled_log_domains = NULL;
static gboolean enable_all_log_domains = FALSE;

static void
e_log_add_domain (const gchar *domain)
{
	g_return_if_fail (domain);

	if (g_strcmp0 (domain, "all") == 0) {
		enable_all_log_domains = TRUE;
		return;
	}

	if (!enabled_log_domains)
		enabled_log_domains = g_ptr_array_new ();

	g_ptr_array_add (enabled_log_domains, g_strdup (domain));
}

/**
 * e_log_get_id:
 * @domain: a domain name
 *
 * Returns the numeric ID corresponding to @domain that can be used with
 * #E_LOG.
 *
 * Return value: A numeric ID corresponding to @domain, or
 *     #E_LOG_INVALID_DOMAIN if the domain was not enabled.
 **/
gint
e_log_get_id (const gchar *domain)
{
	gint i;

	if (!enabled_log_domains && !enable_all_log_domains)
		return E_LOG_INVALID_DOMAIN;

	if (enabled_log_domains) {
		for (i = 0; i < enabled_log_domains->len; i++) {
			if (g_strcmp0 (enabled_log_domains->pdata[i], domain) == 0) {
				return i;
			}
		}
	}

	if (enable_all_log_domains) {
		/* This log domain is not in the list of known domain because
		 * it was not specified in the list of domains to enable, but
		 * "all" was specified. We just add this domain when requested */
		e_log_add_domain (domain);
		return enabled_log_domains->len - 1;
	}

	return E_LOG_INVALID_DOMAIN;
}

/**
 * e_log_set_domains:
 * @domains: domain names to enable
 *
 * Enable the logging for the domains specified in @domains.
 * @domains must be a list of domain names separated by a space, comma,
 * semicolon or colon, or "all" to enable the logging for all the modules.
 **/
void
e_log_set_domains (const gchar *domains)
{
	const gchar *start;
	const gchar *end;
	gchar *domain;

	if (!domains || !*domains)
		return;

	start = domains;

	while (*start) {
		end = strpbrk (start, ",;: \t");
		if (!end)
			end = start + strlen(start);

		domain = g_strndup (start, end - start);
		e_log_add_domain (domain);
		g_free (domain);

		start = end;
		if (*start)
			/* Skip the delimiter. */
			start++;
	}
}

/**
 * e_log_set_level:
 * @log_level: a #GLogLevelFlags
 *
 * Sets the new log level, messages with log level less than @log_level will
 * not be printed.
 * Valid values for @log_level are #G_LOG_LEVEL_ERROR, #G_LOG_LEVEL_CRITICAL,
 * #G_LOG_LEVEL_WARNING, #G_LOG_LEVEL_MESSAGE, #G_LOG_LEVEL_INFO and
 * #G_LOG_LEVEL_DEBUG.
 **/
void
e_log_set_level (GLogLevelFlags log_level)
{
	switch (log_level) {
		case G_LOG_LEVEL_ERROR:
		case G_LOG_LEVEL_CRITICAL:
		case G_LOG_LEVEL_WARNING:
		case G_LOG_LEVEL_MESSAGE:
		case G_LOG_LEVEL_INFO:
		case G_LOG_LEVEL_DEBUG:
			current_log_level = log_level;
			break;
		default:
			g_critical ("Invalid log level");
	}
}

/**
 * e_log_set_level:
 * @str: a string representing the new log level
 *
 * Sets the new log level from @str. See e_log_set_level() for more
 * details.
 * Valid values for @str are "error", "critical", "warning", "message",
 * "info" and "debug". All is also accepted as a synonym for "debug".
 **/
void
e_log_set_level_from_string (const gchar *str)
{
	GLogLevelFlags log_level;

	if (!str || !*str)
		return;

	if (strcmp (str, "error") == 0)
		log_level = G_LOG_LEVEL_ERROR;
	else if (strcmp (str, "critical") == 0)
		log_level = G_LOG_LEVEL_CRITICAL;
	else if (strcmp (str, "warning") == 0)
		log_level = G_LOG_LEVEL_WARNING;
	else if (strcmp (str, "message") == 0)
		log_level = G_LOG_LEVEL_MESSAGE;
	else if (strcmp (str, "info") == 0)
		log_level = G_LOG_LEVEL_INFO;
	else if (strcmp (str, "debug") == 0)
		log_level = G_LOG_LEVEL_DEBUG;
	else if (strcmp (str, "all") == 0)
		/* It's easier to remember "all" then remembering which log
		 * level is the more verbose one, so we provide "all" as
		 * a synonym for "debug". */
		log_level = G_LOG_LEVEL_DEBUG;
	else
		return;

	e_log_set_level (log_level);
}

/**
 * e_log_will_print:
 * @domain_id: an ID for a debugging domain
 * @log_level: a #GLogLevelFlags
 *
 * Returns whether a message with debug domain @domain_id and log level
 * @log_level would be printed. This is useful to avoid doing expensive
 * operations when the result would be discarded.
 *
 * Return value: #TRUE if a message with @domain_id and @log_level would
 * be printed, #FALSE otherwise.
 **/
gboolean
e_log_will_print (gint           domain_id,
		  GLogLevelFlags log_level)
{
	if (domain_id == E_LOG_INVALID_DOMAIN)
		return FALSE;

	g_return_val_if_fail (domain_id >= 0, FALSE);
	g_return_val_if_fail (enabled_log_domains, FALSE);
	g_return_val_if_fail (domain_id < (gint) enabled_log_domains->len,
			FALSE);

	/* ERROR < CRITICAL < WARNING ... */
	return log_level <= current_log_level;
}

/**
 * e_log_real:
 * @domain_id: an ID for a debugging domain
 * @log_level: a #GLogLevelFlags
 * @format: the string to print
 *
 * Internal function used by #E_LOG, you should use #E_LOG to get also the
 * file name and function name in the debug output.
 *
 * Formats the string @format and prints it with g_log() if the debug domain
 * associated with the ID @domain_id is enabled and if @log_level is greater
 * or equal to the log level set with e_log_set_level().
 **/
void
e_log_real (gint           domain_id,
	    GLogLevelFlags log_level,
	    const gchar   *format,
	    ...)
{
	if (e_log_will_print (domain_id, log_level)) {
		gchar *log_domain;
		va_list args;

		log_domain = g_strdup_printf ("eds/%s",
				(gchar*)enabled_log_domains->pdata[domain_id]);
		va_start (args, format);
		g_logv (log_domain, log_level, format, args);
		va_end (args);
		g_free (log_domain);
	}
}
