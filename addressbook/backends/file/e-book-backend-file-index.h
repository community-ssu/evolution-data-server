/*
 * Copyright (C) 2008 Nokia Corporation
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of version 2 of the GNU Lesser General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Rob Bradford <rob@openedhand.com>
 */

#ifndef _E_BOOK_BACKEND_FILE_INDEX
#define _E_BOOK_BACKEND_FILE_INDEX

#include <glib-object.h>
#include "libebook/e-contact.h"
#include "db.h"

G_BEGIN_DECLS

#define E_TYPE_BOOK_BACKEND_FILE_INDEX e_book_backend_file_index_get_type()

#define E_BOOK_BACKEND_FILE_INDEX(obj) \
  (G_TYPE_CHECK_INSTANCE_CAST ((obj), E_TYPE_BOOK_BACKEND_FILE_INDEX, EBookBackendFileIndex))

#define E_BOOK_BACKEND_FILE_INDEX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_BOOK_BACKEND_FILE_INDEX, EBookBackendFileIndexClass))

#define E_IS_BOOK_BACKEND_FILE_INDEX(obj) \
  (G_TYPE_CHECK_INSTANCE_TYPE ((obj), E_TYPE_BOOK_BACKEND_FILE_INDEX))

#define E_IS_BOOK_BACKEND_FILE_INDEX_CLASS(klass) \
  (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_BOOK_BACKEND_FILE_INDEX))

#define E_BOOK_BACKEND_FILE_INDEX_GET_CLASS(obj) \
  (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_BOOK_BACKEND_FILE_INDEX, EBookBackendFileIndexClass))

typedef struct {
  GObject parent;
} EBookBackendFileIndex;

typedef struct {
  GObjectClass parent_class;
} EBookBackendFileIndexClass;

GType e_book_backend_file_index_get_type (void);

EBookBackendFileIndex *e_book_backend_file_index_new (void);
gboolean e_book_backend_file_index_is_usable (EBookBackendFileIndex *index, const gchar *query);
gboolean e_book_backend_file_index_setup_indicies (EBookBackendFileIndex *index, DB *db, 
    const gchar *index_filename);
void e_book_backend_file_index_add_contact (EBookBackendFileIndex *index, EContact *contact);
void e_book_backend_file_index_remove_contact (EBookBackendFileIndex *index, EContact *contact);
void e_book_backend_file_index_modify_contact (EBookBackendFileIndex *index, EContact *old_contact, EContact *new_contact);
void e_book_backend_file_index_sync (EBookBackendFileIndex *index);
GPtrArray *e_book_backend_file_index_query (EBookBackendFileIndex *index, const gchar *query);
G_END_DECLS

#endif /* _E_BOOK_BACKEND_FILE_INDEX */

