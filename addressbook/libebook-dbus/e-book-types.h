/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * A client-side GObject which exposes the
 * Evolution:BookListener interface.
 *
 * Author:
 *   Nat Friedman (nat@ximian.com)
 *
 * Copyright 2000, Ximian, Inc.
 */

#ifndef __E_BOOK_TYPES_H__
#define __E_BOOK_TYPES_H__

#include <glib.h>
#include <libebook/e-contact.h>

G_BEGIN_DECLS

/**
 * E_BOOK_ERROR:
 *
 * Error domain for #EBook. See #GError for more information.
 */
#define E_BOOK_ERROR e_book_error_quark()

/**
 * e_book_error_quark:
 *
 * The implementation of the E_BOOK_ERROR error domain. See #GError for more
 * information.
 *
 * Return value: the associated #GQuark
 */
GQuark e_book_error_quark (void) G_GNUC_CONST;

/**
 * EBookStatus:
 * @E_BOOK_ERROR_OK: there were no errors with the last operation
 * @E_BOOK_ERROR_INVALID_ARG: invalid argument
 * @E_BOOK_ERROR_BUSY: the back-end was busy and could not respond
 * @E_BOOK_ERROR_REPOSITORY_OFFLINE: the back-end could not be reached
 * @E_BOOK_ERROR_NO_SUCH_BOOK: the provided book could not be found
 * @E_BOOK_ERROR_NO_SELF_CONTACT: there was no contact in the book marked as the
 * owner of the book ("self-contact")
 * @E_BOOK_ERROR_SOURCE_NOT_LOADED: the book has not been opened
 * @E_BOOK_ERROR_SOURCE_ALREADY_LOADED: the book is already open (and cannot be
 * re-opened)
 * @E_BOOK_ERROR_PERMISSION_DENIED: permission denied for this book
 * @E_BOOK_ERROR_CONTACT_NOT_FOUND: the specified contact does not exist in this
 * book
 * @E_BOOK_ERROR_CONTACT_ID_ALREADY_EXISTS: the provided UID already exists for
 * a different contact
 * @E_BOOK_ERROR_PROTOCOL_NOT_SUPPORTED: the back-end had a protocol mis-match
 * with the server
 * @E_BOOK_ERROR_CANCELLED: the operation was cancelled before it completed
 * @E_BOOK_ERROR_COULD_NOT_CANCEL: the operation could not be cancelled
 * @E_BOOK_ERROR_AUTHENTICATION_FAILED: authentication with the server failed
 * @E_BOOK_ERROR_AUTHENTICATION_REQUIRED: authentication is required by the
 * server, but was not provided
 * @E_BOOK_ERROR_TLS_NOT_AVAILABLE: the back-end or server does not support TLS
 * encryption
 * @E_BOOK_ERROR_CORBA_EXCEPTION: there was D-Bus-specific error
 * @E_BOOK_ERROR_NO_SUCH_SOURCE: the #ESource does not exist
 * @E_BOOK_ERROR_OFFLINE_UNAVAILABLE: the server is in the process of going
 * offline and is refusing new operations
 * @E_BOOK_ERROR_OTHER_ERROR: there was an error of undetermined nature
 * @E_BOOK_ERROR_INVALID_SERVER_VERSION: the specific versions of the protocol
 * supported by the back-end and server are mis-matched
 * @E_BOOK_ERROR_NO_SPACE: the last operation could not be completed because the
 * required storage space was not available
 * @E_BOOK_ERROR_INVALID_FIELD: one or more of the fields involved is confirmed
 * invalid
 * @E_BOOK_ERROR_METHOD_NOT_SUPPORTED: the operation cannot be performed because
 * the corresponding backend does not support it
 *
 * The final status of the corresponding #EBook operation.
 **/
typedef enum {
	E_BOOK_ERROR_OK,
	E_BOOK_ERROR_INVALID_ARG,
	E_BOOK_ERROR_BUSY,
	E_BOOK_ERROR_REPOSITORY_OFFLINE,
	E_BOOK_ERROR_NO_SUCH_BOOK,
	E_BOOK_ERROR_NO_SELF_CONTACT,
	E_BOOK_ERROR_SOURCE_NOT_LOADED,
	E_BOOK_ERROR_SOURCE_ALREADY_LOADED,
	E_BOOK_ERROR_PERMISSION_DENIED,
	E_BOOK_ERROR_CONTACT_NOT_FOUND,
	E_BOOK_ERROR_CONTACT_ID_ALREADY_EXISTS,
	E_BOOK_ERROR_PROTOCOL_NOT_SUPPORTED,
	E_BOOK_ERROR_CANCELLED,
	E_BOOK_ERROR_COULD_NOT_CANCEL,
	E_BOOK_ERROR_AUTHENTICATION_FAILED,
	E_BOOK_ERROR_AUTHENTICATION_REQUIRED,
	E_BOOK_ERROR_TLS_NOT_AVAILABLE,
	E_BOOK_ERROR_CORBA_EXCEPTION,
	E_BOOK_ERROR_NO_SUCH_SOURCE,
	E_BOOK_ERROR_OFFLINE_UNAVAILABLE,
	E_BOOK_ERROR_OTHER_ERROR,
	E_BOOK_ERROR_INVALID_SERVER_VERSION,
	E_BOOK_ERROR_NO_SPACE,
	E_BOOK_ERROR_INVALID_FIELD,
	E_BOOK_ERROR_METHOD_NOT_SUPPORTED,
} EBookStatus;

/**
 * EBookViewStatus:
 * @E_BOOK_VIEW_STATUS_OK: there were no errors with the last operation
 * @E_BOOK_VIEW_STATUS_TIME_LIMIT_EXCEEDED: the operation timed out
 * @E_BOOK_VIEW_STATUS_SIZE_LIMIT_EXCEEDED: the search results were too large to
 * return
 * @E_BOOK_VIEW_ERROR_INVALID_QUERY: invalid query
 * @E_BOOK_VIEW_ERROR_QUERY_REFUSED: the query was refused by the
 * back-end/server
 * @E_BOOK_VIEW_ERROR_OTHER_ERROR: the last operation failed for an undetermined
 * reason
 *
 * The final status of the corresponding #EBookView operation.
 **/
typedef enum {
	E_BOOK_VIEW_STATUS_OK,
	E_BOOK_VIEW_STATUS_TIME_LIMIT_EXCEEDED,
	E_BOOK_VIEW_STATUS_SIZE_LIMIT_EXCEEDED,
	E_BOOK_VIEW_ERROR_INVALID_QUERY,
	E_BOOK_VIEW_ERROR_QUERY_REFUSED,
	E_BOOK_VIEW_ERROR_OTHER_ERROR
} EBookViewStatus;

typedef enum {
	E_BOOK_CHANGE_CARD_ADDED,
	E_BOOK_CHANGE_CARD_DELETED,
	E_BOOK_CHANGE_CARD_MODIFIED
} EBookChangeType;

typedef struct {
	EBookChangeType  change_type;
	EContact        *contact;
} EBookChange;

/* Convenience defines to save typing */
#define E_PARAM_READABLE G_PARAM_READABLE|G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB
#define E_PARAM_READWRITE G_PARAM_READWRITE|G_PARAM_STATIC_NAME|G_PARAM_STATIC_NICK|G_PARAM_STATIC_BLURB

G_END_DECLS

#endif /* ! __E_BOOK_TYPES_H__ */
