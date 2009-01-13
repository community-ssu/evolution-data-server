/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * Copyright (C) 2006 OpenedHand Ltd
 *
 * This program is free software; you can redistribute it and/or modify it under
 * the terms of version 2 of the GNU Lesser General Public License as published
 * by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software Foundation, Inc.,
 * 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 * Author: Ross Burton <ross@openedhand.com>
 */

#ifndef __E_BOOK_VIEW_PRIVATE_H__
#define __E_BOOK_VIEW_PRIVATE_H__

#include "e-book.h"
#include "e-book-view.h"

EBookView *e_book_view_new (EBook *book, EBookQuery *query, DBusGProxy *view_proxy);

G_END_DECLS

#endif /* ! __E_BOOK_VIEW_PRIVATE_H__ */
