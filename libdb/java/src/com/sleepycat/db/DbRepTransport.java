/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2001-2002
 *      Sleepycat Software.  All rights reserved.
 *
 * $Id: DbRepTransport.java,v 1.1.1.1 2003/11/20 22:13:33 toshok Exp $
 */

package com.sleepycat.db;

/*
 * This is used as a callback by DbEnv.set_rep_transport.
 */
public interface DbRepTransport
{
    public int send(DbEnv env, Dbt control, Dbt rec, int flags, int envid)
        throws DbException;
}
