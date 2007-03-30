/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Blanket header containing the typedefs for object types used in the
 * PAS stuff, so we can disentangle the #includes.
 *
 * Author: Chris Toshok <toshok@ximian.com>
 *
 * Copyright 2003, Ximian, Inc.
 */

#ifndef __E_DATA_BOOK_TYPES_H__
#define __E_DATA_BOOK_TYPES_H__

G_BEGIN_DECLS

typedef struct _EDataBookView        EDataBookView;
typedef struct _EDataBookViewClass   EDataBookViewClass;

typedef struct _EBookBackendSExp EBookBackendSExp;
typedef struct _EBookBackendSExpClass EBookBackendSExpClass;

typedef struct _EBookBackend        EBookBackend;
typedef struct _EBookBackendClass   EBookBackendClass;

typedef struct _EBookBackendSummary EBookBackendSummary;
typedef struct _EBookBackendSummaryClass EBookBackendSummaryClass;

typedef struct _EBookBackendSync        EBookBackendSync;
typedef struct _EBookBackendSyncClass   EBookBackendSyncClass;

typedef struct _EDataBook        EDataBook;
typedef struct _EDataBookClass   EDataBookClass;

typedef enum {
	Success,
	RepositoryOffline,
	PermissionDenied,
	ContactNotFound,
	ContactIdAlreadyExists,
	AuthenticationFailed,
	AuthenticationRequired,
	UnsupportedField,
	UnsupportedAuthenticationMethod,
	TLSNotAvailable,
	NoSuchBook,
	BookRemoved,
	OfflineUnavailable,
	
	/* These can be returned for successful searches, but
	   indicate the result set was truncated */
	SearchSizeLimitExceeded,
	SearchTimeLimitExceeded,
	
	InvalidQuery,
	QueryRefused,
	
	CouldNotCancel,
	
	OtherError,
	InvalidServerVersion,
	NoSpace,
} EDataBookStatus;

/* Some hacks so the backends compile without change */
#define GNOME_Evolution_Addressbook_CallStatus EDataBookStatus
#define GNOME_Evolution_Addressbook_BookMode EDataBookMode
#define GNOME_Evolution_Addressbook_Success Success
#define GNOME_Evolution_Addressbook_ContactNotFound ContactNotFound
#define GNOME_Evolution_Addressbook_NoSpace NoSpace
#define GNOME_Evolution_Addressbook_OtherError OtherError
#define GNOME_Evolution_Addressbook_PermissionDenied PermissionDenied
#define GNOME_Evolution_Addressbook_CouldNotCancel CouldNotCancel

typedef enum {
	Local,
	Remote,
	Any,
} EDataBookMode;

typedef enum {
  E_BOOK_BACKEND_CHANGE_ADDED,
  E_BOOK_BACKEND_CHANGE_DELETED,
  E_BOOK_BACKEND_CHANGE_MODIFIED
} EDataBookChangeType;

typedef struct {
  EDataBookChangeType change_type;
  char *vcard;
} EDataBookChange;

G_END_DECLS

#endif /* __E_DATA_BOOK_TYPES_H__ */
