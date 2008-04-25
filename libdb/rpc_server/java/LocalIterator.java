/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2001-2002
 *      Sleepycat Software.  All rights reserved.
 *
 * $Id: LocalIterator.java,v 1.1.1.1 2003/11/20 22:13:50 toshok Exp $
 */

package com.sleepycat.db.rpcserver;

import java.util.*;

/**
 * Iterator interface.  Note that this matches java.util.Iterator
 * but maintains compatibility with Java 1.1
 * Intentionally package-protected exposure.
 */
interface LocalIterator {
	boolean hasNext();
	Object next();
	void remove();
}
