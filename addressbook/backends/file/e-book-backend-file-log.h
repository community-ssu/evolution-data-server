/*
 * This file is part of eds-backend-file
 *
 * Copyright (C) 2009 Nokia Corporation. All rights reserved.
 *
 * Author: Marco Barisione <marco.barisione@maemo.org>
 */


#ifndef _E_BOOK_BACKEND_FILE_LOG_H__
#define _E_BOOK_BACKEND_FILE_LOG_H__

#include <glib.h>
#include <libedataserver/e-log.h>

extern gint e_book_backend_file_log_domain_id;

#define DEBUG(format, ...) \
	E_LOG (e_book_backend_file_log_domain_id, G_LOG_LEVEL_DEBUG, format, \
	       ##__VA_ARGS__)

#define MESSAGE(format, ...) \
	E_LOG (e_book_backend_file_log_domain_id, G_LOG_LEVEL_MESSAGE, format, \
	       ##__VA_ARGS__)

#define WARNING(format, ...) \
	E_LOG (e_book_backend_file_log_domain_id, G_LOG_LEVEL_WARNING, format, \
	       ##__VA_ARGS__)

#endif /* _E_BOOK_BACKEND_FILE_LOG_H_ */
