# See the file LICENSE for redistribution information.
#
# Copyright (c) 2000-2002
#	Sleepycat Software.  All rights reserved.
#
# $Id: test100.tcl,v 1.1.1.1 2003/11/20 22:14:02 toshok Exp $
#
# TEST	test100
# TEST	Test for functionality near the end of the queue
# TEST	using test025 (DB_APPEND).
proc test100 { method {nentries 10000} {txn -txn} {tnum "100"} args} {
	if { [is_queueext $method ] == 0 } {
		puts "Skipping test0$tnum for $method."
		return;
	}
	eval {test025 $method $nentries 4294967000 $tnum} $args
}
