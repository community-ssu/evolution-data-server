## Process this file with automake to produce Makefile.in

if ENABLE_NNTP
NNTP_DIR=nntp
endif

if ENABLE_IMAPP
IMAPP_DIR=imapp
endif

if OS_WIN32
else
SENDMAIL_DIR=sendmail
endif

# groupwise isn't ported
SUBDIRS = pop3 $(SENDMAIL_DIR) smtp imap imap4 $(NNTP_DIR) local $(IMAPP_DIR) $(GROUPWISE_DIR)
DIST_SUBDIRS = pop3 sendmail smtp imap imap4 nntp local imapp groupwise
