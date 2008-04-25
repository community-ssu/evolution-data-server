/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2000-2002
 *      Sleepycat Software.  All rights reserved.
 *
 * $Id: DbAppendRecno.java,v 1.1.1.1 2003/11/20 22:13:32 toshok Exp $
 */

package com.sleepycat.db;

/*
 * This interface is used by Db.set_append_recno()
 *
 */
public interface DbAppendRecno
{
    public abstract void db_append_recno(Db db, Dbt data, int recno)
        throws DbException;
}

// end of DbAppendRecno.java
