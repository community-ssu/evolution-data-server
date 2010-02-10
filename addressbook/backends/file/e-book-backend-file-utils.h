/*
 * Copyright (C) 2009 Nokia Corporation
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
 * Author: Robert Peto <robert.peto@maemo.org>
 */

#ifndef __E_BOOK_BACKEND_FILE_UTILS_H__
#define __E_BOOK_BACKEND_FILE_UTILS_H__

#include "db.h"
#include <glib.h>

typedef enum {
        TXN_PUT,
        TXN_DEL,
        TXN_INDEX_DEL
} TxnOp;

typedef struct TxnItem {
        TxnOp op;
        DB *db;
        char *key;
        char *value;
} TxnItem;

G_GNUC_INTERNAL
int txn_execute_ops (DB_ENV *env, DB_TXN *parent_txn, GPtrArray *ops);

G_GNUC_INTERNAL
int txn_ops_add_new (GPtrArray *ops, TxnOp op, DB *db, char *key, char *value);

G_GNUC_INTERNAL
void txn_ops_free (GPtrArray *ops);

G_GNUC_INTERNAL
void dbt_fill_with_string (DBT *dbt, const char *str);

G_GNUC_INTERNAL
gboolean indices_need_cleanup (DB_ENV *env);

G_GNUC_INTERNAL
void indices_mark_as_clean (DB_ENV *env);

#endif /* __E_BOOK_BACKEND_FILE_UTILS_H__ */
