/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 *  Copyright (C) 2000-2003 Ximian Inc.
 *
 *  Authors: Michael Zucchi <notzed@ximian.com>
 *           Jeffrey Stedfast <fejj@ximian.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */


#ifndef _CAMEL_MIME_UTILS_H
#define _CAMEL_MIME_UTILS_H

#ifdef __cplusplus
extern "C" {
#pragma }
#endif /* __cplusplus */

#include <time.h>
#include <glib.h>

/* maximum recommended size of a line from camel_header_fold() */
#define CAMEL_FOLD_SIZE (77)
/* maximum hard size of a line from camel_header_fold() */
#define CAMEL_FOLD_MAX_SIZE (998)

/* a list of references for this message */
struct _camel_header_references {
	struct _camel_header_references *next;
	char *id;
};

struct _camel_header_param {
	struct _camel_header_param *next;
	char *name;
	char *value;
};

/* describes a content-type */
typedef struct {
	char *type;
	char *subtype;
	struct _camel_header_param *params;
	unsigned int refcount;
} CamelContentType;

/* a raw rfc822 header */
/* the value MUST be US-ASCII */
struct _camel_header_raw {
	struct _camel_header_raw *next;
	char *name;
	char *value;
	int offset;		/* in file, if known */
};

typedef struct _CamelContentDisposition {
	char *disposition;
	struct _camel_header_param *params;
	unsigned int refcount;
} CamelContentDisposition;

enum _camel_header_address_t {
	CAMEL_HEADER_ADDRESS_NONE,	/* uninitialised */
	CAMEL_HEADER_ADDRESS_NAME,
	CAMEL_HEADER_ADDRESS_GROUP
};

struct _camel_header_address {
	struct _camel_header_address *next;
	enum _camel_header_address_t type;
	char *name;
	union {
		char *addr;
		struct _camel_header_address *members;
	} v;
	unsigned int refcount;
};

struct _camel_header_newsgroup {
	struct _camel_header_newsgroup *next;

	char *newsgroup;
};

/* Address lists */
struct _camel_header_address *camel_header_address_new (void);
struct _camel_header_address *camel_header_address_new_name (const char *name, const char *addr);
struct _camel_header_address *camel_header_address_new_group (const char *name);
void camel_header_address_ref (struct _camel_header_address *);
void camel_header_address_unref (struct _camel_header_address *);
void camel_header_address_set_name (struct _camel_header_address *, const char *name);
void camel_header_address_set_addr (struct _camel_header_address *, const char *addr);
void camel_header_address_set_members (struct _camel_header_address *, struct _camel_header_address *group);
void camel_header_address_list_clear (struct _camel_header_address **);

struct _camel_header_address *camel_header_address_decode (const char *in, const char *charset);

/* fold a header */
char *camel_header_address_fold (const char *in, size_t headerlen);

/* encode a phrase, like the real name of an address */
char *camel_header_encode_phrase (const unsigned char *in);

/* camel ctype type functions for rfc822/rfc2047/other, which are non-locale specific */
enum {
	CAMEL_MIME_IS_CTRL		= 1<<0,
	CAMEL_MIME_IS_LWSP		= 1<<1,
	CAMEL_MIME_IS_TSPECIAL	= 1<<2,
	CAMEL_MIME_IS_SPECIAL	= 1<<3,
	CAMEL_MIME_IS_SPACE	= 1<<4,
	CAMEL_MIME_IS_DSPECIAL	= 1<<5,
	CAMEL_MIME_IS_QPSAFE	= 1<<6,
	CAMEL_MIME_IS_ESAFE	= 1<<7,	/* encoded word safe */
	CAMEL_MIME_IS_PSAFE	= 1<<8,	/* encoded word in phrase safe */
	CAMEL_MIME_IS_ATTRCHAR  = 1<<9,	/* attribute-char safe (rfc2184) */
};

extern unsigned short camel_mime_special_table[256];

#define camel_mime_is_ctrl(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_CTRL) != 0)
#define camel_mime_is_lwsp(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_LWSP) != 0)
#define camel_mime_is_tspecial(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_TSPECIAL) != 0)
#define camel_mime_is_type(x, t) ((camel_mime_special_table[(unsigned char)(x)] & (t)) != 0)
#define camel_mime_is_ttoken(x) ((camel_mime_special_table[(unsigned char)(x)] & (CAMEL_MIME_IS_TSPECIAL|CAMEL_MIME_IS_LWSP|CAMEL_MIME_IS_CTRL)) == 0)
#define camel_mime_is_atom(x) ((camel_mime_special_table[(unsigned char)(x)] & (CAMEL_MIME_IS_SPECIAL|CAMEL_MIME_IS_SPACE|CAMEL_MIME_IS_CTRL)) == 0)
#define camel_mime_is_dtext(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_DSPECIAL) == 0)
#define camel_mime_is_fieldname(x) ((camel_mime_special_table[(unsigned char)(x)] & (CAMEL_MIME_IS_CTRL|CAMEL_MIME_IS_SPACE)) == 0)
#define camel_mime_is_qpsafe(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_QPSAFE) != 0)
#define camel_mime_is_especial(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_ESPECIAL) != 0)
#define camel_mime_is_psafe(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_PSAFE) != 0)
#define camel_mime_is_attrchar(x) ((camel_mime_special_table[(unsigned char)(x)] & CAMEL_MIME_IS_ATTRCHAR) != 0)

#ifdef __cplusplus
}
#endif /* __cplusplus */

#endif /* ! _CAMEL_MIME_UTILS_H */
