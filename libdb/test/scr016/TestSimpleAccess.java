/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 1997-2002
 *	Sleepycat Software.  All rights reserved.
 *
 * $Id: TestSimpleAccess.java,v 1.1.1.1 2003/11/20 22:14:06 toshok Exp $
 */

/*
 * Simple test for get/put of specific values.
 */

package com.sleepycat.test;

import com.sleepycat.db.*;
import java.io.FileNotFoundException;

public class TestSimpleAccess
{
    public static void main(String[] args)
    {
        try {
            Db db = new Db(null, 0);
            db.open(null, "my.db", null, Db.DB_BTREE, Db.DB_CREATE, 0644);

            TestUtil.populate(db);
            System.out.println("finished test");
        }
        catch (DbException dbe) {
            System.err.println("Db Exception: " + dbe);
        }
        catch (FileNotFoundException fnfe) {
            System.err.println("FileNotFoundException: " + fnfe);
        }
    }
}
