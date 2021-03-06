# See the file LICENSE for redistribution information.
#
# Copyright (c) 1999-2002
#	Sleepycat Software.  All rights reserved.
#
# $Id: test069.tcl,v 1.1.1.1 2003/11/20 22:14:01 toshok Exp $
#
# TEST	test069
# TEST	Test of DB_CURRENT partial puts without duplicates-- test067 w/
# TEST	small ndups to ensure that partial puts to DB_CURRENT work
# TEST	correctly in the absence of duplicate pages.
proc test069 { method {ndups 50} {tnum 69} args } {
	eval test067 $method $ndups $tnum $args
}
