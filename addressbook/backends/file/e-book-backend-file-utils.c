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

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "e-book-backend-file-log.h"
#include "e-book-backend-file-utils.h"

/* maximum retry to commit a transaction */
#define MAX_RETRY 10
/* wait 200ms before retry the transaction */
#define RETRY_WAIT 200000


/**
 * Fills up @dbt with the @str value.
 * If @str is NULL, then @dbt's values are only erased (filled with 0s)
 * NOTE: it copies only the pointer value of @str,
 *       caller should allocate and release its memory
 */
void
dbt_fill_with_string (DBT *dbt, const char *str)
{
        memset (dbt, 0, sizeof (DBT));
        if (str) {
                dbt->data = (void *) str;
                dbt->size = strlen (str) + 1;
                dbt->flags = DB_DBT_USERMEM;
        }
}


/* helper function that deletes a matching key/value pair from db with a cursor */
static int
txn_delete_by_cursor (DB *db, DB_TXN *tid, DBT *key, DBT *value)
{
        int db_error;
        int dbc_error = 0;
        DBC *dbc;

        g_return_val_if_fail (db, EINVAL);
        g_return_val_if_fail (tid, EINVAL);
        g_return_val_if_fail (key, EINVAL);
        g_return_val_if_fail (value, EINVAL);

        db_error = db->cursor (db, tid, &dbc, 0);
        if (db_error != 0) {
                WARNING ("db->cursor failed: %s", db_strerror (db_error));
                return db_error;
        }

        /* This is weird, but just as DB_SET, DB_GET_BOTH seems to need
         * memory to allocate the returned value
         * (which is the same as we pass in) */
        value->flags = DB_DBT_MALLOC;
        dbc_error = dbc->c_get (dbc, key, value, DB_GET_BOTH);
        if (dbc_error != 0) {
                WARNING ("dbc->c_get failed: %s", db_strerror (dbc_error));
                goto out;
        }
        /* We are not really interessted in the value, as we know it
         * already */
        g_free (value->data);

        dbc_error = dbc->c_del (dbc, 0);
        if (dbc_error != 0) {
                WARNING ("dbc->del failed: %s", db_strerror (dbc_error));
                goto out;
        }

out:
        db_error = dbc->c_close (dbc);
        if (db_error != 0) {
                WARNING ("dbc->close failed: %s", db_strerror (db_error));
        }

        return dbc_error ? dbc_error : db_error;
}

/* helper function that executes a single db operation in a transaction */
static int
do_db_operation (DB_TXN *tid, TxnItem *txn_item)
{
        int db_error;
        DBT key, value;

        g_return_val_if_fail (txn_item, EINVAL);
        g_return_val_if_fail (txn_item->db, EINVAL);

        dbt_fill_with_string (&key, txn_item->key);
        dbt_fill_with_string (&value, txn_item->value);

        switch (txn_item->op) {
                case TXN_PUT:
                        /* insert */
                        db_error = txn_item->db->put (txn_item->db, tid, &key, &value, 0);
                        if (db_error != 0) {
                                WARNING ("DB->put failed: %s",
                                                db_strerror (db_error));
                        }
                        return db_error;

                case TXN_DEL:
                        /* delete */
                        db_error = txn_item->db->del (txn_item->db, tid, &key, 0);
                        if (db_error != 0) {
                                WARNING ("DB->del failed: %s",
                                                db_strerror (db_error));
                        }
                        return db_error;

                case TXN_INDEX_DEL:
                        /* delete with cursor */
                        return txn_delete_by_cursor (txn_item->db, tid, &key, &value);

                default:
                        WARNING ("invalid TxnItem->op");
                        return EINVAL;
        }

        g_assert_not_reached ();
}

/**
 * Creates a transaction and commits it from the previously assembled
 * @ops array. If the commit wasn't successful then the transaction will
 * be aborted and retried MAX_RETRY times.
 *
 * @env: a database environment
 * @parent_txn: the parent transaction, it can be NULL. (if @parent_txn is given,
 * then this transaction will be its child, that means if parent is aborted,
 * then this transaction aborts as well.)
 * @ops: the previously assembled array with #txn_ops_add_new()
 *
 * Returns: 0 on success
 */
int
txn_execute_ops (DB_ENV *env, DB_TXN *parent_txn, GPtrArray *ops)
{
        int db_error;
        int i, fail = 0;
        DB_TXN *tid;

        g_return_val_if_fail (env, EINVAL);
        g_return_val_if_fail (ops, EINVAL);

RETRY:
        DEBUG ("try #%d", fail + 1);

        /* begin transaction */
        db_error = env->txn_begin (env, parent_txn, &tid, 0);
        if (db_error != 0) {
                WARNING ("txn_begin failed: %s", db_strerror (db_error));
                return db_error;
        }

        /* iterate over ops, and execute it */
        for (i = 0; i < ops->len; i++) {
                TxnItem *txn_item = g_ptr_array_index (ops, i);

                if (txn_item && (db_error = do_db_operation (tid, txn_item)) != 0) {
                        /* abort and retry */
                        int db_err;
                        db_err = tid->abort (tid);
                        if (db_err != 0) {
                                WARNING ("DB_TXN->abort failed: %s",
                                                db_strerror (db_err));
                                return db_err;
                        }

                        if (++fail < MAX_RETRY && db_error == DB_LOCK_DEADLOCK) {
                                usleep (RETRY_WAIT);
                                goto RETRY;
                        }
                        else {
                                WARNING ("MAX_RETRY exceeded or unhandled db error");
                                return db_error;
                        }
                }
        }

        /* commit transaction */
        db_error = tid->commit (tid, 0);
        if (db_error != 0) {
                WARNING ("DB_TXN->commit failed: %s", db_strerror (db_error));
                return db_error;
        }

        /* success */
        return 0;
}

/**
 * Creates a new #TxnItem struct and adds it to @ops.
 *
 * @key and @value must be allocated before this function gets called,
 * they will be owned by TxnItem struct, and should be release by
 * #txn_ops_free() function.
 *
 * @ops: must be a valid GPtrArray. This will hold the requested
 * transaction operations.
 * @op: the requested operation, see #TxnOp enum
 * @key: a string value that describes the key, it must be allocated.
 * @value: a string value that describes the value, it must be allocated.
 *
 * Returns: 0 on success
 */
int
txn_ops_add_new (GPtrArray *ops, TxnOp op, DB *db, char *key, char *value)
{
        TxnItem *txn_item;

        g_return_val_if_fail (ops, EINVAL);
        g_return_val_if_fail (db, EINVAL);
        /* key and/or value can be NULL in certain circumstances */

        txn_item = g_slice_new (TxnItem);
        txn_item->op = op;
        txn_item->db = db;
        txn_item->key = key;
        txn_item->value = value;

        g_ptr_array_add (ops, txn_item);

        return 0;
}

/* helper function to free up the memory that is allocated to each TxnItem. */
static void
txn_item_free (TxnItem *txn_item)
{
        if (!txn_item)
                return;

        g_free (txn_item->key);
        g_free (txn_item->value);

        g_slice_free (TxnItem, txn_item);
}

/**
 * Frees the GPtrArray and the stored #TxnItem structs.
 */
void
txn_ops_free (GPtrArray *ops)
{
        if (!ops)
                return;

        g_ptr_array_foreach (ops, (GFunc)txn_item_free, NULL);
        g_ptr_array_free (ops, TRUE);
}

