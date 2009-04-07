#ifndef __E_BOOK_LOG_H__
#define __E_BOOK_LOG_H__

#include <glib.h>

G_BEGIN_DECLS

#define E_LOG_INVALID_DOMAIN -1

#define E_LOG(domain_id, log_level, format, ...) \
	e_log_real (domain_id, log_level, \
		"%s at %s: " format, G_STRFUNC, G_STRLOC, \
		##__VA_ARGS__)

gint          e_log_get_id                (const gchar    *domain);
void          e_log_set_domains           (const gchar    *domains);
void          e_log_set_level             (GLogLevelFlags  log_level);
void          e_log_set_level_from_string (const gchar    *str);
gboolean      e_log_will_print            (gint            domain_id,
					   GLogLevelFlags  log_level);
void          e_log_real                  (gint            domain_id,
					   GLogLevelFlags  log_level,
					   const gchar    *format,
					   ...);

G_END_DECLS

#endif /* ! __E_BOOK_LOG_H__ */
