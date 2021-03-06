/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* camel-nntp-auth.h : authentication for nntp */

/* 
 *
 * Author : Chris Toshok <toshok@ximian.com> 
 *
 * Copyright (C) 1999 Ximian .
 *
 * This program is free software; you can redistribute it and/or 
 * modify it under the terms of version 2 of the GNU Lesser General Public 
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
 * USA
 */


#ifndef CAMEL_NNTP_AUTH_H
#define CAMEL_NNTP_AUTH_H 1

#include <camel-nntp-store.h>

#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus }*/

int camel_nntp_auth_authenticate (CamelNNTPStore *store, CamelException *ex);

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* CAMEL_NNTP_AUTH_H */
