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

#ifndef __E_DATA_BOOK_FACTORY_H__
#define __E_DATA_BOOK_FACTORY_H__

#include <glib-object.h>

typedef struct EDataBookFactory EDataBookFactory;
typedef struct EDataBookFactoryClass EDataBookFactoryClass;
typedef struct _EDataBookFactoryPrivate EDataBookFactoryPrivate;

struct EDataBookFactory
{
  GObject parent;
  EDataBookFactoryPrivate *priv;
};

struct EDataBookFactoryClass
{
  GObjectClass parent;
};

#define E_TYPE_DATA_BOOK_FACTORY              (e_data_book_factory_get_type ())
#define E_DATA_BOOK_FACTORY(object)           (G_TYPE_CHECK_INSTANCE_CAST ((object), E_TYPE_DATA_BOOK_FACTORY, EDataBookFactory))
#define E_DATA_BOOK_FACTORY_CLASS(klass)      (G_TYPE_CHECK_CLASS_CAST ((klass), E_TYPE_DATA_BOOK_FACTORY, EDataBookFactoryClass))
#define E_IS_DATA_BOOK_FACTORY(object)        (G_TYPE_CHECK_INSTANCE_TYPE ((object), E_TYPE_DATA_BOOK_FACTORY))
#define E_IS_DATA_BOOK_FACTORY_CLASS(klass)   (G_TYPE_CHECK_CLASS_TYPE ((klass), E_TYPE_DATA_BOOK_FACTORY))
#define E_DATA_BOOK_FACTORY_GET_CLASS(obj)    (G_TYPE_INSTANCE_GET_CLASS ((obj), E_TYPE_DATA_BOOK_FACTORY, EDataBookFactoryClass))

typedef enum
{
  E_DATA_BOOK_FACTORY_ERROR_GENERIC
} EDataBookFactoryError;

GQuark e_data_book_factory_error_quark (void);
#define E_DATA_BOOK_FACTORY_ERROR e_data_book_factory_error_quark ()

GType e_data_book_factory_get_type (void);

#endif /* __E_DATA_BOOK_FACTORY_H__ */
