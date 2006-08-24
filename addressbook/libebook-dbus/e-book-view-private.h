/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * The Evolution addressbook client object.
 *
 * Author: Ross Burton <ross@o-hand.com>
 *
 * Copyright 2005 OpenedHand
 */

#ifndef __E_BOOK_VIEW_PRIVATE_H__
#define __E_BOOK_VIEW_PRIVATE_H__

#include "e-book.h"
#include "e-book-view.h"

EBookView *e_book_view_new (EBook *book, DBusGProxy *view_proxy, EBookViewListener *listener);

G_END_DECLS

#endif /* ! __E_BOOK_VIEW_PRIVATE_H__ */
