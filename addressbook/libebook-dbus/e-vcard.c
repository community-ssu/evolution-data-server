/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/* e-vcard.c
 *
 * Copyright (C) 2003 Ximian, Inc.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of version 2 of the GNU Lesser General Public
 * License as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 *
 * Author: Chris Toshok (toshok@ximian.com)
 */

/* This file implements the decoding of the v-card format
 * http://www.imc.org/pdi/vcard-21.txt
 */

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include "e-vcard.h"

#define d(x)

#define CRLF "\r\n"

#define FOLD_LINES 0

/** Encoding used in v-card 
 *  Note: v-card spec defines additional 7BIT 8BIT and X- encoding
 */
typedef enum {
	EVC_ENCODING_RAW,    /* no encoding */
	EVC_ENCODING_BASE64, /* base64 */
	EVC_ENCODING_QP      /* quoted-printable */
} EVCardEncoding;

struct _EVCardPrivate {
	GList    *attributes;
	char     *vcard;
	unsigned  parsing : 1;
};

struct _EVCardAttribute {
	char  *group;
	char  *name;
	GList *params; /* EVCardParam */
	GList *values;
	GList *decoded_values;
	EVCardEncoding encoding;
	gboolean encoding_set;
};

struct _EVCardAttributeParam {
	char     *name;
	GList    *values;  /* GList of char*'s*/
};

static GObjectClass *parent_class;

static void   _evc_base64_init(void);
static size_t _evc_base64_encode_step(unsigned char *in, size_t len, gboolean break_lines, unsigned char *out, int *state, int *save);
static size_t _evc_base64_decode_step(unsigned char *in, size_t len, unsigned char *out, int *state, unsigned int *save);
static size_t _evc_base64_decode_simple (char *data, size_t len);
static char  *_evc_base64_encode_simple (const char *data, size_t len);

static char * _evc_quoted_printable_decode (const char *input);
static char * _evc_quoted_printable_encode (const char *input, size_t size, gboolean *is_qp);
static char * _evc_escape_string_21 (const char *s);

static void e_vcard_attribute_param_add_value_with_len (EVCardAttributeParam *param, const char *value, int length);

static void
e_vcard_dispose (GObject *object)
{
	EVCard *evc = E_VCARD (object);

	if (evc->priv) {
		/* Directly access priv->attributes and don't call e_vcard_ensure_attributes(),
 		 * since it is pointless to start vCard parsing that late. */
		g_list_foreach (evc->priv->attributes, (GFunc)e_vcard_attribute_free, NULL);
		g_list_free (evc->priv->attributes);
		g_free (evc->priv->vcard);

		g_free (evc->priv);
		evc->priv = NULL;
	}

	if (G_OBJECT_CLASS (parent_class)->dispose)
		G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
e_contact_add_attribute_cb (EVCard *evc, EVCardAttribute *attr)
{
}

static void
e_contact_remove_attribute_cb (EVCard *evc, EVCardAttribute *attr)
{
}

static void
e_vcard_class_init (EVCardClass *klass)
{
	GObjectClass *object_class;

	object_class = G_OBJECT_CLASS(klass);

	parent_class = g_type_class_ref (G_TYPE_OBJECT);

	object_class->dispose = e_vcard_dispose;

	klass->add_attribute = e_contact_add_attribute_cb;
	klass->remove_attribute = e_contact_remove_attribute_cb;

	_evc_base64_init();
}

static void
e_vcard_init (EVCard *evc)
{
	evc->priv = g_new0 (EVCardPrivate, 1);
}

GType
e_vcard_get_type (void)
{
	static GType vcard_type = 0;

	if (!vcard_type) {
		static const GTypeInfo vcard_info =  {
			sizeof (EVCardClass),
			NULL,           /* base_init */
			NULL,           /* base_finalize */
			(GClassInitFunc) e_vcard_class_init,
			NULL,           /* class_finalize */
			NULL,           /* class_data */
			sizeof (EVCard),
			0,             /* n_preallocs */
			(GInstanceInitFunc) e_vcard_init,
		};

		vcard_type = g_type_register_static (G_TYPE_OBJECT, "EVCard", &vcard_info, 0);
	}

	return vcard_type;
}

static void
skip_to_next_line (const char **p)
{
	const char *lp = *p;

	while (*lp != '\r' && *lp != '\n' && *lp != '\0')
		lp++;

	if (*lp == '\r') {
		if (*++lp == '\n')
			lp++;
	}
	
	*p = lp;
}

/* skip forward until we hit a character in @s, CRLF, or \0.  leave *p
   pointing at the character that causes us to stop */
static void
skip_until (const char **p, char *s)
{
	const char *lp;

	lp = *p;

	while (*lp != '\r' && *lp != '\0') {
		gboolean s_matches = FALSE;
		char *ls;
		for (ls = s; *ls; ls = g_utf8_next_char (ls)) {
			if (g_utf8_get_char (ls) == g_utf8_get_char (lp)) {
				s_matches = TRUE;
				break;
			}
		}

		if (s_matches)
			break;
		lp = g_utf8_next_char (lp);
	}

	*p = lp;
}

#define WRITE_CHUNK \
	if (chunk_start) { \
		g_string_append_len (str, chunk_start, lp - chunk_start); \
		chunk_start = NULL; \
	}

#define UNFOLD(p)				       \
	else if (*p == '\r' || *p == '\n') {	       \
		p++;				       \
		if (*p == '\n' || *p == '\r') {	       \
			p++;			       \
			if (*p == ' ' || *p == '\t') { \
				p++;		       \
			} else {		       \
				break;		       \
			}			       \
		} else if (*p == ' ' || *p == '\t') {  \
			p++;			       \
		} else {			       \
			break;			       \
		}				       \
	}

#define UNFOLD_CHUNK(p)				       \
	else if (*p == '\r' || *p == '\n') {	       \
		WRITE_CHUNK;			       \
		p++;				       \
		if (*p == '\n' || *p == '\r') {	       \
			p++;			       \
			if (*p == ' ' || *p == '\t') { \
				p++;		       \
			} else {		       \
				break;		       \
			}			       \
		} else if (*p == ' ' || *p == '\t') {  \
			p++;			       \
		} else {			       \
			break;			       \
		}				       \
	}


static gboolean
read_attribute_value (EVCardAttribute *attr, const char **p, gboolean quoted_printable)
{
	const char *lp = *p;
	GString *str;
	const char *chunk_start = NULL;
	gint count = 0;

	/* read in the value */
	str = g_string_sized_new (16);
	while (*lp && (count < 40)) {
		//g_printerr("(%c)", *lp);
		if (*lp == '=' && quoted_printable) {
			char a, b;
			WRITE_CHUNK;
			if ((a = *(++lp)) == '\0') break;
			if ((b = *(++lp)) == '\0') break;
			else if (isxdigit(a) && isxdigit (b)) {
                                char c;

				a = tolower (a);
				b = tolower (b);

				c = (((a>='a'?a-'a'+10:a-'0')&0x0f) << 4)
					| ((b>='a'?b-'a'+10:b-'0')&0x0f);

                                g_string_append_c (str, c);
			}
			else 
				{
					g_string_append_c (str, a);
					g_string_append_c (str, b);
				}
			
			lp++;
		}
		else if (*lp == '\\') {
			WRITE_CHUNK;
			/* convert back to the non-escaped version of
			   the characters */
			lp = g_utf8_next_char(lp);
			if (*lp == '\0') {
				g_string_append_c (str, '\\');
				break;
			}
			switch (*lp) {
			case 'n': g_string_append_c (str, '\n'); break;
			case 'r': g_string_append_c (str, '\r'); break;
			case ';': g_string_append_c (str, ';'); break;
			case ',': g_string_append_c (str, ','); break;
			case '\\': g_string_append_c (str, '\\'); break;
			default:
				g_warning ("invalid escape, passing it through");
				g_string_append_c (str, '\\');
				g_string_insert_unichar (str, -1, g_utf8_get_char(lp));
				break;
			}
			lp = g_utf8_next_char(lp);
		}
		else if ((*lp == ';') ||
			 (*lp == ',' && !strcmp (attr->name, "CATEGORIES"))) {
			WRITE_CHUNK;
			e_vcard_attribute_add_value (attr, str->str);
			g_string_assign (str, "");
			lp = g_utf8_next_char(lp);
			count++;
		}
		UNFOLD_CHUNK(lp)
		else {
			if (chunk_start == NULL)
				chunk_start = lp;

			//g_string_insert_unichar (str, -1, g_utf8_get_char (lp));
			lp = g_utf8_next_char(lp);
		}
	}

	if (count == 40)
	{	
		g_warning (G_STRLOC ": Excessive VCARD detected. "
		    "More than 40 values for attribute.");
		return FALSE;
	}

	WRITE_CHUNK;
	if (str) {
		e_vcard_attribute_add_value (attr, str->str);
		g_string_free (str, TRUE);
	}

	*p = lp;
	return TRUE;
}

static gboolean
read_attribute_params (EVCardAttribute *attr, const char **p, gboolean *quoted_printable)
{
	const char *lp = *p;
	GString *str;
	EVCardAttributeParam *param = NULL;
	gboolean in_quote = FALSE;
	gint count = 0;

	str = g_string_sized_new (16);
	while (*lp != '\0' && (count < 20)) {
		if (*lp == '"') {
			in_quote = !in_quote;
			lp = g_utf8_next_char (lp);
		}
		else if (in_quote || g_unichar_isalnum (g_utf8_get_char (lp)) || *lp == '-' || *lp == '_') {
			g_string_insert_unichar (str, -1, g_utf8_get_char (lp));
			lp = g_utf8_next_char (lp);
		}
		/* accumulate until we hit the '=' or ';'.  If we hit
		 * a '=' the string contains the parameter name.  if
		 * we hit a ';' the string contains the parameter
		 * value and the name is either ENCODING (if value ==
		 * QUOTED-PRINTABLE) or TYPE (in any other case.)
		 */
		else if (*lp == '=') {
			if (str->len > 0) {
				param = e_vcard_attribute_param_new (str->str);
				g_string_assign (str, "");
				lp = g_utf8_next_char (lp);
			}
			else {
				skip_until (&lp, ":;");
				if (*lp == '\r') {
					lp = g_utf8_next_char (lp); /* \n */
					lp = g_utf8_next_char (lp); /* start of the next line */
					break;
				}
				else if (*lp == ';')
					lp = g_utf8_next_char (lp);
			}
		}
		else if (*lp == ';' || *lp == ':' || *lp == ',') {
			gboolean colon = (*lp == ':');
			gboolean comma = (*lp == ',');

			if (param) {
				if (str->len > 0) {
					e_vcard_attribute_param_add_value_with_len (param, str->str, str->len);
					g_string_assign (str, "");
					if (!colon)
						lp = g_utf8_next_char (lp);
				}
				else {
					/* we've got a parameter of the form:
					 * PARAM=(.*,)?[:;]
					 * so what we do depends on if there are already values
					 * for the parameter.  If there are, we just finish
					 * this parameter and skip past the offending character
					 * (unless it's the ':'). If there aren't values, we free
					 * the parameter then skip past the character.
					 */
					if (!param->values) {
						e_vcard_attribute_param_free (param);
						param = NULL;
						if (!colon)
							lp = g_utf8_next_char (lp);
					}
				}

				if (param
				    && !strcmp (param->name, "ENCODING")
				    && !g_ascii_strcasecmp (param->values->data, "quoted-printable")) {
					*quoted_printable = TRUE;
					e_vcard_attribute_param_free (param);
					param = NULL;
				}
			}
			else {
				if (str->len > 0) {
					char *param_name;
					if (!g_ascii_strcasecmp (str->str,
								 "quoted-printable")) {
						param_name = "ENCODING";
						*quoted_printable = TRUE;
					}
					/* apple's broken addressbook app outputs naked BASE64
					   parameters, which aren't even vcard 3.0 compliant. */
					else if (!g_ascii_strcasecmp (str->str,
								      "base64")) {
						param_name = "ENCODING";
						g_string_assign (str, "b");
					}
					else {
						param_name = "TYPE";
					}

					if (param_name) {
						param = e_vcard_attribute_param_new (param_name);
						e_vcard_attribute_param_add_value_with_len (param, str->str, str->len);
					}
					g_string_assign (str, "");
					if (!colon)
						lp = g_utf8_next_char (lp);
				}
				else {
					/* we've got an attribute with a truly empty
					   attribute parameter.  So it's of the form:
					   
					   ATTR;[PARAM=value;]*;[PARAM=value;]*:

					   (note the extra ';')

					   the only thing to do here is, well.. nothing.
					   we skip over the character if it's not a colon,
					   and the rest is handled for us: We'll either
					   continue through the loop again if we hit a ';',
					   or we'll break out correct below if it was a ':' */
					if (!colon)
						lp = g_utf8_next_char (lp);
				}
			}
			if (param && !comma) {
				e_vcard_attribute_add_param (attr, param);
				count++;
				param = NULL;
			}
			if (colon)
				break;
		}
		UNFOLD (lp)
		else {
			g_warning ("invalid character found in parameter spec");
			g_string_assign (str, "");
			skip_until (&lp, ":;");
			
			if (*lp == '\r') {
				lp = g_utf8_next_char (lp); /* \n */
				lp = g_utf8_next_char (lp); /* start of the next line */
				break;
			}
		}
	}

	if (count == 20)
	{
		g_warning (G_STRLOC ": Excessive VCARD detected. "
		    "More than 20 parameters found.");
		return FALSE;
	}

	if (str)
		g_string_free (str, TRUE);

	attr->params = g_list_reverse (attr->params);
	
	*p = lp;

	return TRUE;
}

/* reads an entire attribute from the input buffer, leaving p pointing
   at the start of the next line (past the \r\n) */
static EVCardAttribute*
read_attribute (const char **p)
{
	char *attr_group = NULL;
	char *attr_name = NULL;
	EVCardAttribute *attr = NULL;
	GString *str;
	const char *lp = *p;
	char c;
	gboolean is_qp = FALSE;
	gunichar uc;
	gint count = 0;

	/* first read in the group/name */
	str = g_string_sized_new (16);
	while ((c = *lp) && (count < 200)) {
		if (c == ':' || c == ';') {
			if (str->len != 0) {
				/* we've got a name, break out to the value/attribute parsing */
				attr_name = g_string_free (str, FALSE);
				break;
			}
			else {
				/* a line of the form:
				 * (group.)?[:;]
				 *
				 * since we don't have an attribute
				 * name, skip to the end of the line
				 * and try again.
				 */
				g_string_free (str, TRUE);
				*p = lp;
				skip_to_next_line(p);
				goto lose;
			}
		}
		else if (c == '.') {
			if (attr_group) {
				g_warning ("extra `.' in attribute specification.  ignoring extra group `%s'",
					   str->str);
				g_string_assign (str, "");
			}
			if (str->len != 0) {
				attr_group = g_string_free (str, FALSE);
				str = g_string_new ("");
			}
		}
		else if (g_unichar_isalnum ((uc = g_utf8_get_char (lp))) ||c == '-' || c == '_') {
			g_string_insert_unichar (str, -1, g_unichar_toupper (uc));
		}
		UNFOLD (lp)
		else {
			g_warning ("invalid character '%c' found in attribute group/name", c);
			g_string_free (str, TRUE);
			*p = lp;
			skip_to_next_line(p);
			goto lose;
		}

		lp = g_utf8_next_char(lp);
		count++;
	}

	if (count == 200) {
		g_warning (G_STRLOC ": Excessive VCARD detected. " 
		    "More than 200 characters before attribute parameters.");
		*p = lp;
		skip_to_next_line(p);
		goto lose;
	}

	if (!attr_name) {
		*p = lp;
		skip_to_next_line(p);
		goto lose;
	}

	attr = e_vcard_attribute_new (attr_group, attr_name);
	g_free (attr_group);
	g_free (attr_name);

	if (*lp == ';') {
		/* skip past the ';' */
		lp = g_utf8_next_char(lp);
		if (!read_attribute_params (attr, &lp, &is_qp)) {
			*p = lp;
			skip_to_next_line(p);
			goto lose;
		}

		if (is_qp)
			attr->encoding = EVC_ENCODING_RAW;
	}
	if (*lp == ':') {
		/* skip past the ':' */
		lp = g_utf8_next_char(lp);
		if (!read_attribute_value (attr, &lp, is_qp))
		{
			goto lose;
		}
	}

	*p = lp;

	if (!attr->values) {
		goto lose;
	}

	return attr;
 lose:
	if (attr)
		e_vcard_attribute_free (attr);
	return NULL;
}

static void
parse (EVCard *evc, const char *str, gboolean ignore_uid)
{
	EVCardAttribute *attr;
	gint count = 0;

	if (str == NULL)
		return;

	attr = read_attribute (&str);
	if (!attr || attr->group || strcmp (attr->name, "BEGIN")) {
		g_warning ("vcard began without a BEGIN:VCARD\n");
	}
	if (attr) {
		if (!strcmp (attr->name, "BEGIN") ||
		    (ignore_uid && !strcmp (attr->name, EVC_UID)))
			e_vcard_attribute_free (attr);
		else
			evc->priv->attributes = g_list_prepend (evc->priv->attributes, attr);
	}

	gboolean seen_end = FALSE;
	for (; (*str) && count < 50; count++) {
		attr = read_attribute (&str);
		if (G_UNLIKELY (!attr)) {
			g_warning ("Couldn't parse attribute, ignoring line.");
			continue;
		}
		if (G_UNLIKELY (0 == strcmp (attr->name, "END"))) {
			seen_end = TRUE;
			e_vcard_attribute_free (attr);
			break;
		}
		/* start of a new vcard, stop parsing this one */
		if (G_UNLIKELY (0 == strcmp (attr->name, "BEGIN"))) {
			e_vcard_attribute_free (attr);
			break;
		}

		if (G_UNLIKELY (ignore_uid && 0 == strcmp (attr->name, EVC_UID))) {
			e_vcard_attribute_free (attr);
			continue;
		}
		evc->priv->attributes = g_list_prepend (evc->priv->attributes, attr);
	}

	if (count == 50) {
		 g_warning (G_STRLOC ": Excessive VCARD detected. "
		     "More than 50 attributes detected");
	}

	if (!seen_end)
		g_warning ("vcard ended without END:VCARD\n");

	evc->priv->attributes = g_list_reverse (evc->priv->attributes);
}

static GList*
e_vcard_ensure_attributes (EVCard *evc)
{
	if (evc->priv->vcard) {
		gboolean have_uid = (NULL != evc->priv->attributes);
		char *vcs = evc->priv->vcard;

		if (have_uid) {
			EVCardAttribute *attr = evc->priv->attributes->data;

			g_assert (NULL == evc->priv->attributes->next);
			g_assert (0 == strcmp (attr->name, EVC_UID));
		}

		/* detach vCard to avoid loops */
		evc->priv->parsing = TRUE;
		evc->priv->vcard = NULL;

		parse (evc, vcs, have_uid);
		g_free (vcs);

		evc->priv->parsing = FALSE;
	}

	return evc->priv->attributes;
}

/**
 * e_vcard_escape_string:
 * @s: the string to escape
 *
 * Escapes a string according to RFC2426, section 5.
 *
 * Return value: A newly allocated, escaped string.
 **/
char*
e_vcard_escape_string (const char *s)
{
	GString *str;
	const char *p;

	str = g_string_new ("");

	/* Escape a string as described in RFC2426, section 5 */
	for (p = s; p && *p; p++) {
		switch (*p) {
		case '\n':
			g_string_append (str, "\\n");
			break;
		case '\r':
			if (*(p+1) == '\n')
				p++;
			g_string_append (str, "\\n");
			break;
		case ';':
			g_string_append (str, "\\;");
			break;
		case ',':
			g_string_append (str, "\\,");
			break;
		case '\\':
			g_string_append (str, "\\\\");
			break;
		default:
			g_string_append_c (str, *p);
			break;
		}
	}

	return g_string_free (str, FALSE);
}

/**
 * e_vcard_unescape_string:
 * @s: the string to unescape
 *
 * Unescapes a string according to RFC2426, section 5.
 *
 * Return value: A newly allocated, unescaped string.
 **/
char*
e_vcard_unescape_string (const char *s)
{
	GString *str;
	const char *p;

	g_return_val_if_fail (s != NULL, NULL);

	str = g_string_new ("");

	/* Unescape a string as described in RFC2426, section 5 */
	for (p = s; *p; p++) {
		if (*p == '\\') {
			p++;
			if (*p == '\0') {
				g_string_append_c (str, '\\');
				break;
			}
			switch (*p) {
			case 'n':  g_string_append_c (str, '\n'); break;
			case 'r':  g_string_append_c (str, '\r'); break;
			case ';':  g_string_append_c (str, ';'); break;
			case ',':  g_string_append_c (str, ','); break;
			case '\\': g_string_append_c (str, '\\'); break;
			default:
				g_warning ("invalid escape, passing it through");
				g_string_append_c (str, '\\');
				g_string_append_unichar (str, g_utf8_get_char(p));
				break;
			}
		}
	}

	return g_string_free (str, FALSE);
}

void
e_vcard_construct_with_uid (EVCard *evc, const char *str, const char *uid)
{
	g_return_if_fail (E_IS_VCARD (evc));
	g_return_if_fail (str != NULL);

	g_return_if_fail (NULL == evc->priv->vcard);
	g_return_if_fail (NULL == evc->priv->attributes);

	if (*str)
		evc->priv->vcard = g_strdup (str);

        if (uid) {
                EVCardAttribute *attr;

                attr = e_vcard_attribute_new (NULL, EVC_UID);
                e_vcard_attribute_add_value (attr, uid);

                evc->priv->attributes = g_list_prepend (evc->priv->attributes, attr);
        }
}

void
e_vcard_construct (EVCard *evc, const char *str)
{
        e_vcard_construct_with_uid (evc, str, NULL);
}

/**
 * e_vcard_new:
 * 
 * Creates a new, blank #EVCard.
 *
 * Return value: A new, blank #EVCard.
 **/
EVCard *
e_vcard_new (void)
{
	return e_vcard_new_from_string ("");
}

/**
 * e_vcard_new_from_string:
 * @str: a string representation of the vcard to create
 *
 * Creates a new #EVCard from the passed-in string
 * representation.
 *
 * Return value: A new #EVCard.
 **/
EVCard *
e_vcard_new_from_string (const char *str)
{
	EVCard *evc;

	g_return_val_if_fail (str != NULL, NULL);

	evc = g_object_new (E_TYPE_VCARD, NULL);

	e_vcard_construct (evc, str);

	return evc;
}


static char*
e_vcard_to_string_vcard_21 (EVCard *evc)
{
        GList *l;
        GList *v;
        GString *str;

        str = g_string_new ("BEGIN:VCARD" CRLF "VERSION:2.1" CRLF);

	for (l = e_vcard_ensure_attributes (evc); l; l = l->next) {
		GList *p;
		EVCardAttribute *attr = l->data;
		GString *attr_str;
                GString *value_str;
                gboolean qp_set = FALSE;

                /* Skip the version attribute since we are outputting to
                 * version 2.1 */
		if (!strcmp (attr->name, "VERSION"))
			continue;

		attr_str = g_string_new ("");

		/* Groupping */
		if (attr->group) {
			g_string_append (attr_str, attr->group);
			g_string_append_c (attr_str, '.');
		}
		g_string_append (attr_str, attr->name);

                if (attr->encoding_set) {
                        switch (attr->encoding) {
                                case EVC_ENCODING_BASE64:
                                        g_string_append (attr_str, ";ENCODING=BASE64");
                                        break;
                                case EVC_ENCODING_QP:
                                        g_string_append (attr_str, ";ENCODING=QUOTED-PRINTABLE");
                                        qp_set = TRUE;
                                        break;
                                default:
                                        g_warning ("wrong encoding type");
                        }
                }
		/* handle the parameters */
		for (p = attr->params; p; p = p->next) {
			EVCardAttributeParam *param = p->data;

                        /* skip the encoding parameter */
                        if (!g_ascii_strcasecmp (param->name, EVC_ENCODING))
                                continue;

                        g_string_append_c (attr_str, ';');

                        /* only append TYPE parameter's name iff it's PHOTO or LOGO */
                        if (!g_ascii_strcasecmp (param->name, EVC_TYPE)) {
                                if (!g_ascii_strcasecmp (attr->name, EVC_PHOTO) ||
                                    !g_ascii_strcasecmp (attr->name, EVC_LOGO)) {
                                        g_string_append (attr_str, param->name);
                                        if (param->values)
                                                g_string_append_c (attr_str, '=');
                                }
                        }
                        else {
                                g_string_append (attr_str, param->name);
                                if (param->values)
                                        g_string_append_c (attr_str, '=');
                        }

			if (param->values) {
				for (v = param->values; v; v = v->next) {
					char *value = v->data;
					char *p = value;
					gboolean quotes = FALSE;
					while (*p) {
                                                /* values like UTF-8 shouldn't be quoted */
						if (!g_unichar_isalnum (g_utf8_get_char (p)) && *p != '-') {
							quotes = TRUE;
							break;
						}
						p = g_utf8_next_char (p);
					}
					if (quotes)
						g_string_append_c (attr_str, '"');
					g_string_append (attr_str, value);
					if (quotes)
						g_string_append_c (attr_str, '"');
					if (v->next)
						g_string_append_c (attr_str, ';');
				}
			}
		}

                /* handle the values */
                value_str = g_string_new (":");
		for (v = attr->values; v; v = v->next) {
			char *value = v->data;
			char *escaped_value = NULL;

                        escaped_value = _evc_escape_string_21 (value);

                        if (attr->encoding == EVC_ENCODING_RAW) {
                                gboolean is_qp = FALSE;
                                char *qp_str = _evc_quoted_printable_encode (escaped_value, strlen (escaped_value), &is_qp);
                                if (is_qp && !qp_set) {
                                        attr_str = g_string_append (attr_str, ";ENCODING=QUOTED-PRINTABLE");
                                        qp_set = TRUE;
                                }
                                g_string_append (value_str, qp_str);
                                g_free (qp_str);
                        }
                        else {
			        g_string_append (value_str, escaped_value);
                        }
			if (v->next) {
				/* XXX toshok - i hate you, rfc 2426.
				   why doesn't CATEGORIES use a ; like
				   a normal list attribute? */
				if (!strcmp (attr->name, "CATEGORIES"))
					g_string_append_c (value_str, ',');
				else
					g_string_append_c (value_str, ';');
			}

			g_free (escaped_value);
		}
                g_string_append (attr_str, value_str->str);
                g_string_free (value_str, TRUE);

		/* lines longer than 75 characters SHOULD be folded */
                if (attr_str->len > 75) {
                        int l = 0;

                        do {
                                /* insert soft line break */
                                if ((attr_str->len - l) > 75) {
                                        l += 75;

                                        if (qp_set) {
                                                /* we don't want to break line inside an encoded value */
                                                if (*(attr_str->str + l-1) == '=')
                                                        l -= 1;
                                                else if (*(attr_str->str + l-2) == '=')
                                                        l -= 2;

                                                g_string_insert_len (attr_str, l, "="CRLF" ", sizeof ("="CRLF" ") - 1);
                                                l += sizeof ("="CRLF" "); /* because of the inserted characters */
                                        }
                                        else {
                                                g_string_insert_len (attr_str, l, CRLF" ", sizeof (CRLF" ") - 1);
                                                l += sizeof (CRLF" ");
                                        }
                                }
                                else {
                                        break;
                                }
                        } while (l < attr_str->len);
                }

                if (attr->encoding != EVC_ENCODING_BASE64) {
		        g_string_append_printf (str, "%s"CRLF, attr_str->str);
                }
                else {
                        g_string_append_printf (str, "%s"CRLF CRLF, attr_str->str);
                }
		g_string_free (attr_str, TRUE);
	}

	g_string_append (str, "END:VCARD");

	return g_string_free (str, FALSE);
}

static char*
e_vcard_to_string_vcard_30 (EVCard *evc)
{
	GList *l;
	GList *v;

	GString *str;

	str = g_string_new ("BEGIN:VCARD" CRLF);

	/* we hardcode the version (since we're outputting to a
	   specific version) and ignore any version attributes the
	   vcard might contain */
	g_string_append (str, "VERSION:3.0" CRLF);

	for (l = e_vcard_ensure_attributes (evc); l; l = l->next) {
		GList *p;
		EVCardAttribute *attr = l->data;
		GString *attr_str;

		if (!strcmp (attr->name, "VERSION"))
			continue;

		attr_str = g_string_new ("");

		/* From rfc2425, 5.8.2
		 *
		 * contentline  = [group "."] name *(";" param) ":" value CRLF
		 */

		if (attr->group) {
			g_string_append (attr_str, attr->group);
			g_string_append_c (attr_str, '.');
		}
		g_string_append (attr_str, attr->name);

		/* handle the parameters */
		for (p = attr->params; p; p = p->next) {
			EVCardAttributeParam *param = p->data;
			/* 5.8.2:
			 * param        = param-name "=" param-value *("," param-value)
			 */
			g_string_append_c (attr_str, ';');
			g_string_append (attr_str, param->name);
			if (param->values) {
				g_string_append_c (attr_str, '=');
				for (v = param->values; v; v = v->next) {
					char *value = v->data;
					char *p = value;
					gboolean quotes = FALSE;
					while (*p) {
						if (!g_unichar_isalnum (g_utf8_get_char (p))) {
							quotes = TRUE;
							break;
						}
						p = g_utf8_next_char (p);
					}
					if (quotes)
						g_string_append_c (attr_str, '"');
					g_string_append (attr_str, value);
					if (quotes)
						g_string_append_c (attr_str, '"');
					if (v->next)
						g_string_append_c (attr_str, ',');
				}
			}
		}

		g_string_append_c (attr_str, ':');

		for (v = attr->values; v; v = v->next) {
			char *value = v->data;
			char *escaped_value = NULL;

			escaped_value = e_vcard_escape_string (value);

			g_string_append (attr_str, escaped_value);
			if (v->next) {
				/* XXX toshok - i hate you, rfc 2426.
				   why doesn't CATEGORIES use a ; like
				   a normal list attribute? */
				if (!strcmp (attr->name, "CATEGORIES"))
					g_string_append_c (attr_str, ',');
				else
					g_string_append_c (attr_str, ';');
			}

			g_free (escaped_value);
		}

		/* 5.8.2:
		 * When generating a content line, lines longer than 75
		 * characters SHOULD be folded
		 */
#if FOLD_LINES
		l = 0;
		do {
			
			if ((g_utf8_strlen (attr_str->str, -1) -l) > 75) {
				char *p;

				l += 75;
				p = g_utf8_offset_to_pointer (attr_str->str, l);
			
				g_string_insert_len (attr_str, (p - attr_str->str), CRLF " ", sizeof (CRLF " ") - 1);
			}
			else
				break;
		} while (l < g_utf8_strlen (attr_str->str, -1));
#endif

		g_string_append (attr_str, CRLF);

		g_string_append (str, attr_str->str);
		g_string_free (attr_str, TRUE);
	}

	g_string_append (str, "END:VCARD");

	return g_string_free (str, FALSE);
}

/**
 * e_vcard_to_string:
 * @evc: the #EVCard to export
 * @format: the format to export to
 *
 * Exports @evc to a string representation, specified
 * by the @format argument.
 *
 * Return value: A newly allocated string representing the vcard.
 **/
char*
e_vcard_to_string (EVCard *evc, EVCardFormat format)
{
	g_return_val_if_fail (E_IS_VCARD (evc), NULL);

	switch (format) {
	case EVC_FORMAT_VCARD_21:
		return e_vcard_to_string_vcard_21 (evc);
	case EVC_FORMAT_VCARD_30:
		return e_vcard_to_string_vcard_30 (evc);
	default:
		g_warning ("invalid format specifier passed to e_vcard_to_string");
		return g_strdup ("");
	}
}

/**
 * e_vcard_dump_structure:
 * @evc: the #EVCard to dump
 *
 * Prints a dump of @evc's structure to stdout. Used for
 * debugging.
 **/
void
e_vcard_dump_structure (EVCard *evc)
{
	GList *a;
	GList *v;
	int i;

	g_return_if_fail (E_IS_VCARD (evc));

	printf ("vCard\n");
	for (a = e_vcard_ensure_attributes (evc); a; a = a->next) {
		GList *p;
		EVCardAttribute *attr = a->data;
		printf ("+-- %s\n", attr->name);
		if (attr->params) {
			printf ("    +- params=\n");

			for (p = attr->params, i = 0; p; p = p->next, i++) {
				EVCardAttributeParam *param = p->data;
				printf ("    |   [%d] = %s", i,param->name);
				printf ("(");
				for (v = param->values; v; v = v->next) {
					char *value = e_vcard_escape_string ((char*)v->data);
					printf ("%s", value);
					if (v->next)
						printf (",");
					g_free (value);
				}

				printf (")\n");
			}
		}
		printf ("    +- values=\n");
		for (v = attr->values, i = 0; v; v = v->next, i++) {
			printf ("        [%d] = `%s'\n", i, (char*)v->data);
		}
	}
}

/**
 * e_vcard_is_parsed:
 * @evc: an #EVCard
 *
 * Check if the @evc has been parsed already. Used for debugging.
 *
 * Return: %TRUE if @evc has been parsed, %FALSE otherwise.
 **/
gboolean
e_vcard_is_parsed (EVCard *evc)
{
	g_return_val_if_fail (E_IS_VCARD (evc), FALSE);
	return (!evc->priv->vcard && evc->priv->attributes);
}

/**
 * e_vcard_is_parsing:
 * @evc: an #EVCard
 *
 * Check if the @evc is currently beening parsed. Used in derived classes to
 * distinguish between parser caused attribute additions and real attribute
 * additions.
 *
 * Return: %TRUE if @evc is currently beening parsed, %FALSE otherwise.
 **/
gboolean
e_vcard_is_parsing (EVCard *evc)
{
	g_return_val_if_fail (E_IS_VCARD (evc), FALSE);
	return evc->priv->parsing;
}

GType
e_vcard_attribute_get_type (void)
{
	static GType type_id = 0;

	if (!type_id)
		type_id = g_boxed_type_register_static ("EVCardAttribute",
							(GBoxedCopyFunc) e_vcard_attribute_copy,
							(GBoxedFreeFunc) e_vcard_attribute_free);
	return type_id;
}

/**
 * e_vcard_attribute_new:
 * @attr_group: a group name
 * @attr_name: an attribute name
 *
 * Creates a new #EVCardAttribute with the specified group and
 * attribute names.
 *
 * Return value: A new #EVCardAttribute.
 **/
EVCardAttribute*
e_vcard_attribute_new (const char *attr_group, const char *attr_name)
{
	EVCardAttribute *attr;

	attr = g_slice_new0 (EVCardAttribute);

	attr->group = g_strdup (attr_group);
	attr->name = g_ascii_strup (attr_name, -1);

	return attr;
}

/**
 * e_vcard_attribute_free:
 * @attr: attribute to free
 *
 * Frees an attribute, its values and its parameters.
 **/
void
e_vcard_attribute_free (EVCardAttribute *attr)
{
	g_return_if_fail (attr != NULL);

	g_free (attr->group);
	g_free (attr->name);

	e_vcard_attribute_remove_values (attr);

	e_vcard_attribute_remove_params (attr);

	g_slice_free (EVCardAttribute, attr);
}

/**
 * e_vcard_attribute_copy:
 * @attr: attribute to copy
 *
 * Makes a copy of @attr.
 *
 * Return value: A new #EVCardAttribute identical to @attr.
 **/
EVCardAttribute*
e_vcard_attribute_copy (EVCardAttribute *attr)
{
	EVCardAttribute *a;
	GList *p;

	g_return_val_if_fail (attr != NULL, NULL);

	a = e_vcard_attribute_new (attr->group, attr->name);

	for (p = attr->values; p; p = p->next)
		e_vcard_attribute_add_value (a, p->data);

	for (p = attr->params; p; p = p->next)
		e_vcard_attribute_add_param (a, e_vcard_attribute_param_copy (p->data));

	return a;
}

/**
 * e_vcard_remove_attributes:
 * @evc: vcard object
 * @attr_group: group name of attributes to be removed
 * @attr_name: name of the arributes to be removed
 *
 * Removes all the attributes with group name and attribute name equal to 
 * passed in values. If @attr_group is %NULL or an empty string,
 * it removes all the attributes with passed in name irrespective of
 * their group names.
 **/
void
e_vcard_remove_attributes (EVCard *evc, const char *attr_group, const char *attr_name)
{
	EVCardClass *klass;
	GList *attr;

	g_return_if_fail (E_IS_VCARD (evc));
	g_return_if_fail (attr_name != NULL);

	klass = E_VCARD_GET_CLASS (evc);
	attr = e_vcard_ensure_attributes (evc);

	while (attr) {
		GList *next_attr;
		EVCardAttribute *a = attr->data;

		next_attr = attr->next;

		if (((!attr_group || *attr_group == '\0') ||
		     (attr_group && !g_ascii_strcasecmp (attr_group, a->group))) &&
		    ((!attr_name && !a->name) || !g_ascii_strcasecmp (attr_name, a->name))) {

			/* matches, remove/delete the attribute */
			evc->priv->attributes = g_list_delete_link (evc->priv->attributes, attr);
			klass->remove_attribute (evc, a);
			e_vcard_attribute_free (a);
		}

		attr = next_attr;
	}
}

/**
 * e_vcard_remove_attribute:
 * @evc: an #EVCard
 * @attr: an #EVCardAttribute to remove
 *
 * Removes @attr from @evc and frees it.
 **/
void
e_vcard_remove_attribute (EVCard *evc, EVCardAttribute *attr)
{
	g_return_if_fail (E_IS_VCARD (evc));
	g_return_if_fail (attr != NULL);

	/* No need to call e_vcard_ensure_attributes() here: That function has been
 	 * called already if this is a valid call and attr is one our attributes.
	 */
	evc->priv->attributes = g_list_remove (evc->priv->attributes, attr);
	E_VCARD_GET_CLASS (evc)->remove_attribute (evc, attr);
	e_vcard_attribute_free (attr);
}

/**
 * e_vcard_add_attribute:
 * @evc: an #EVCard
 * @attr: an #EVCardAttribute to add
 *
 * Adds @attr to @evc.
 **/
void
e_vcard_add_attribute (EVCard *evc, EVCardAttribute *attr)
{
	g_return_if_fail (E_IS_VCARD (evc));
	g_return_if_fail (attr != NULL);

	evc->priv->attributes = g_list_prepend (e_vcard_ensure_attributes (evc), attr);

	E_VCARD_GET_CLASS (evc)->add_attribute (evc, attr);
}

/**
 * e_vcard_add_attribute_with_value:
 * @evcard: an #EVCard
 * @attr: an #EVCardAttribute to add
 * @value: a value to assign to the attribute
 *
 * Adds @attr to @evcard, setting it to @value.
 **/
void
e_vcard_add_attribute_with_value (EVCard *evcard,
				  EVCardAttribute *attr, const char *value)
{
	g_return_if_fail (E_IS_VCARD (evcard));
	g_return_if_fail (attr != NULL);

	e_vcard_attribute_add_value (attr, value);

	e_vcard_add_attribute (evcard, attr);
}

/**
 * e_vcard_add_attribute_with_values:
 * @evcard: an @EVCard
 * @attr: an #EVCardAttribute to add
 * @Varargs: a %NULL-terminated list of values to assign to the attribute
 *
 * Adds @attr to @evcard, assigning the list of values to it.
 **/
void
e_vcard_add_attribute_with_values (EVCard *evcard, EVCardAttribute *attr, ...)
{
	va_list ap;
	char *v;

	g_return_if_fail (E_IS_VCARD (evcard));
	g_return_if_fail (attr != NULL);

	va_start (ap, attr);

	while ((v = va_arg (ap, char*))) {
		e_vcard_attribute_add_value (attr, v);
	}

	va_end (ap);

	e_vcard_add_attribute (evcard, attr);
}

/**
 * e_vcard_attribute_add_value:
 * @attr: an #EVCardAttribute
 * @value: a string value
 *
 * Adds @value to @attr's list of values.
 **/
void
e_vcard_attribute_add_value (EVCardAttribute *attr, const char *value)
{
	g_return_if_fail (attr != NULL);
	g_return_if_fail (value != NULL);

	attr->values = g_list_append (attr->values, g_strdup (value));
}

/**
 * e_vcard_attribute_add_value_decoded:
 * @attr: an #EVCardAttribute
 * @value: an encoded value
 * @len: the length of the encoded value, in bytes
 *
 * Decodes @value according to the encoding used for @attr, and
 * adds it to @attr's list of values.
 **/
void
e_vcard_attribute_add_value_decoded (EVCardAttribute *attr, const char *value, int len)
{
	g_return_if_fail (attr != NULL);

	switch (attr->encoding) {
	case EVC_ENCODING_RAW:
		g_warning ("can't add_value_decoded with an attribute using RAW encoding.  you must set the ENCODING parameter first");
		break;
	case EVC_ENCODING_BASE64: {
		char *b64_data = _evc_base64_encode_simple (value, len);
		GString *decoded = g_string_new_len (value, len);

		/* make sure the decoded list is up to date */
		e_vcard_attribute_get_values_decoded (attr);

		d(printf ("base64 encoded value: %s\n", b64_data));
		d(printf ("original length: %d\n", len));

		attr->values = g_list_append (attr->values, b64_data);
		attr->decoded_values = g_list_append (attr->decoded_values, decoded);
		break;
	}
	case EVC_ENCODING_QP: {
                char *escaped = _evc_escape_string_21 (value);
                char *qp_data = _evc_quoted_printable_encode (escaped, strlen (escaped), NULL);
                GString *decoded = g_string_new_len (value, len);
                g_free (escaped);

                /* make sure the decoded list is up to date */
                e_vcard_attribute_get_values_decoded (attr);

                attr->values = g_list_append (attr->values, qp_data);
                attr->decoded_values = g_list_append (attr->decoded_values, decoded);

		break;
	}
        }
}

/**
 * e_vcard_attribute_add_values:
 * @attr: an #EVCardAttribute
 * @Varargs: a %NULL-terminated list of strings
 *
 * Adds a list of values to @attr.
 **/
void
e_vcard_attribute_add_values (EVCardAttribute *attr,
			      ...)
{
	va_list ap;
	char *v;

	g_return_if_fail (attr != NULL);

	va_start (ap, attr);

	while ((v = va_arg (ap, char*))) {
		e_vcard_attribute_add_value (attr, v);
	}

	va_end (ap);
}

static void
free_gstring (GString *str)
{
	g_string_free (str, TRUE);
}

/**
 * e_vcard_attribute_remove_values:
 * @attr: an #EVCardAttribute
 *
 * Removes all values from @attr.
 **/
void
e_vcard_attribute_remove_values (EVCardAttribute *attr)
{
	g_return_if_fail (attr != NULL);

	g_list_foreach (attr->values, (GFunc)g_free, NULL);
	g_list_free (attr->values);
	attr->values = NULL;

	g_list_foreach (attr->decoded_values, (GFunc)free_gstring, NULL);
	g_list_free (attr->decoded_values);
	attr->decoded_values = NULL;
}

/**
 * e_vcard_attribute_remove_value:
 * @attr: an #EVCardAttribute
 * @s: an value to remove
 *
 * Removes from the value list in @attr the value @s.
 **/
void
e_vcard_attribute_remove_value (EVCardAttribute *attr, const char *s)
{
	GList *l;

	g_return_if_fail (attr != NULL);
	g_return_if_fail (s != NULL);

	l = g_list_find_custom (attr->values, s, (GCompareFunc)strcmp);
	if (l == NULL) {
		return;
	}
	
	attr->values = g_list_delete_link (attr->values, l);
}

/**
 * e_vcard_attribute_remove_param:
 * @attr: an #EVCardAttribute
 * @param_name: a parameter name
 *
 * Removes the parameter @param_name from the attribute @attr.
 */
void
e_vcard_attribute_remove_param (EVCardAttribute *attr, const char *param_name)
{
	GList *l;
	EVCardAttributeParam *param;

	g_return_if_fail (attr != NULL);
	g_return_if_fail (param_name != NULL);
	
	for (l = attr->params; l; l = l->next) {
		param = l->data;
		if (g_ascii_strcasecmp (e_vcard_attribute_param_get_name (param),
					param_name) == 0) {
			attr->params = g_list_delete_link (attr->params, l);
			e_vcard_attribute_param_free(param);
			break;
		}
	}
}

/**
 * e_vcard_attribute_remove_params:
 * @attr: an #EVCardAttribute
 *
 * Removes all parameters from @attr.
 **/
void
e_vcard_attribute_remove_params (EVCardAttribute *attr)
{
	g_return_if_fail (attr != NULL);

	g_list_foreach (attr->params, (GFunc)e_vcard_attribute_param_free, NULL);
	g_list_free (attr->params);
	attr->params = NULL;

	/* also remove the cached encoding on this attribute */
	attr->encoding_set = FALSE;
	attr->encoding = EVC_ENCODING_RAW;
}

/**
 * e_vcard_attribute_param_new:
 * @name: the name of the new parameter
 *
 * Creates a new parameter named @name.
 *
 * Return value: A new #EVCardAttributeParam.
 **/
EVCardAttributeParam*
e_vcard_attribute_param_new (const char *name)
{
	EVCardAttributeParam *param = g_slice_new (EVCardAttributeParam);
	param->values = NULL;
	param->name = g_ascii_strup (name, -1);

	return param;
}

/**
 * e_vcard_attribute_param_free:
 * @param: an #EVCardAttributeParam
 *
 * Frees @param and its values.
 **/
void
e_vcard_attribute_param_free (EVCardAttributeParam *param)
{
	g_return_if_fail (param != NULL);

	g_free (param->name);

	e_vcard_attribute_param_remove_values (param);

	g_slice_free (EVCardAttributeParam, param);
}

/**
 * e_vcard_attribute_param_copy:
 * @param: an #EVCardAttributeParam
 *
 * Makes a copy of @param.
 *
 * Return value: a new #EVCardAttributeParam identical to @param.
 **/
EVCardAttributeParam*
e_vcard_attribute_param_copy (EVCardAttributeParam *param)
{
	EVCardAttributeParam *p;
	GList *l;

	g_return_val_if_fail (param != NULL, NULL);

	p = e_vcard_attribute_param_new (e_vcard_attribute_param_get_name (param));

	for (l = param->values; l; l = l->next) {
		e_vcard_attribute_param_add_value (p, l->data);
	}

	return p;
}

/**
 * e_vcard_attribute_add_param:
 * @attr: an #EVCardAttribute
 * @param: an #EVCardAttributeParam to add
 *
 * Adds @param to @attr's list of parameters.
 **/
void
e_vcard_attribute_add_param (EVCardAttribute *attr,
			     EVCardAttributeParam *param)
{
	g_return_if_fail (attr != NULL);
	g_return_if_fail (param != NULL);

	attr->params = g_list_prepend (attr->params, param);

	/* we handle our special encoding stuff here */

	if (!strcmp (param->name, EVC_ENCODING)) {
		if (attr->encoding_set) {
			g_warning ("ENCODING specified twice");
			return;
		}

		if (param->values && param->values->data) {
			if (!g_ascii_strcasecmp ((char*)param->values->data, "b") ||
			    !g_ascii_strcasecmp ((char*)param->values->data, "BASE64"))
				attr->encoding = EVC_ENCODING_BASE64;
			else if (!g_ascii_strcasecmp ((char*)param->values->data, EVC_QUOTEDPRINTABLE))
				attr->encoding = EVC_ENCODING_QP;
			else {
				g_warning ("Unknown value `%s' for ENCODING parameter.  values will be treated as raw",
					   (char*)param->values->data);
			}

			attr->encoding_set = TRUE;
		}
		else {
			g_warning ("ENCODING parameter added with no value");
		}
	}
}

/**
 * e_vcard_attribute_param_add_value:
 * @param: an #EVCardAttributeParam
 * @value: a string value to add
 *
 * Adds @value to @param's list of values.
 **/
void
e_vcard_attribute_param_add_value (EVCardAttributeParam *param,
				   const char *value)
{
	g_return_if_fail (param != NULL);

	param->values = g_list_append (param->values, g_strdup (value));
}

/**
 * e_vcard_attribute_param_add_value_with_len:
 * @param: an #EVCardAttributeParam
 * @value: a string value to add
 * @length: the length of @string
 *
 * Adds @value to @param's list of values.  This function is for internal use
 * only.
 **/
static void
e_vcard_attribute_param_add_value_with_len (EVCardAttributeParam *param,
					    const char *value,
					    int length)
{
	g_return_if_fail (param != NULL);

	param->values = g_list_append (param->values, g_strndup (value, length));
}

/**
 * e_vcard_attribute_param_add_values:
 * @param: an #EVCardAttributeParam
 * @Varargs: a %NULL-terminated list of strings
 *
 * Adds a list of values to @param.
 **/
void
e_vcard_attribute_param_add_values (EVCardAttributeParam *param,
				    ...)
{
	va_list ap;
	char *v;

	g_return_if_fail (param != NULL);

	va_start (ap, param);

	while ((v = va_arg (ap, char*))) {
		e_vcard_attribute_param_add_value (param, v);
	}

	va_end (ap);
}

/**
 * e_vcard_attribute_add_param_with_value:
 * @attr: an #EVCardAttribute
 * @param: an #EVCardAttributeParam
 * @value: a string value
 *
 * Adds @value to @param, then adds @param to @attr.
 **/
void
e_vcard_attribute_add_param_with_value (EVCardAttribute *attr,
					EVCardAttributeParam *param, const char *value)
{
	g_return_if_fail (attr != NULL);
	g_return_if_fail (param != NULL);

	e_vcard_attribute_param_add_value (param, value);

	e_vcard_attribute_add_param (attr, param);
}

/**
 * e_vcard_attribute_add_param_with_values:
 * @attr: an #EVCardAttribute
 * @param: an #EVCardAttributeParam
 * @Varargs: a %NULL-terminated list of strings
 *
 * Adds the list of values to @param, then adds @param
 * to @attr.
 **/
void
e_vcard_attribute_add_param_with_values (EVCardAttribute *attr,
					 EVCardAttributeParam *param, ...)
{
	va_list ap;
	char *v;

	g_return_if_fail (attr != NULL);
	g_return_if_fail (param != NULL);

	va_start (ap, param);

	while ((v = va_arg (ap, char*))) {
		e_vcard_attribute_param_add_value (param, v);
	}

	va_end (ap);

	e_vcard_attribute_add_param (attr, param);
}

/**
 * e_vcard_attribute_param_remove_values:
 * @param: an #EVCardAttributeParam
 *
 * Removes and frees all values from @param.
 **/
void
e_vcard_attribute_param_remove_values (EVCardAttributeParam *param)
{
	g_return_if_fail (param != NULL);

	g_list_foreach (param->values, (GFunc)g_free, NULL);
	g_list_free (param->values);
	param->values = NULL;
}

/**
 * e_vcard_attribute_remove_param_value:
 * @attr: an #EVCardAttribute
 * @param_name: a parameter name
 * @s: a value
 *
 * Removes the value @s from the parameter @param_name on the attribute @attr.
 **/
void
e_vcard_attribute_remove_param_value (EVCardAttribute *attr, const char *param_name, const char *s)
{
	GList *l;
	EVCardAttributeParam *param;

	g_return_if_fail (attr != NULL);
	g_return_if_fail (param_name != NULL);
	g_return_if_fail (s != NULL);

	for (l = attr->params; l; l = l->next) {
		param = l->data;
		if (g_ascii_strcasecmp (e_vcard_attribute_param_get_name (param), param_name) == 0) {
			goto found;
		}
	}
	return;

 found:
	l = g_list_find_custom (param->values, s, (GCompareFunc)strcmp);
	if (l == NULL) {
		return;
	}
	
	param->values = g_list_delete_link (param->values, l);
	if (param->values == NULL) {
		e_vcard_attribute_param_free (param);
		attr->params = g_list_remove (attr->params, param);
	}
}

/**
 * e_vcard_get_attributes:
 * @evcard: an #EVCard
 *
 * Gets the list of attributes from @evcard. The list and its
 * contents are owned by @evcard, and must not be freed.
 *
 * Return value: A list of attributes of type #EVCardAttribute.
 **/
GList*
e_vcard_get_attributes (EVCard *evcard)
{
	g_return_val_if_fail (E_IS_VCARD (evcard), NULL);

	return e_vcard_ensure_attributes (evcard);
}

/**
 * e_vcard_get_attribute:
 * @evc: an #EVCard
 * @name: the name of the attribute to get
 *
 * Get the attribute @name from @evc.  The #EVCardAttribute is owned by
 * @evcard and should not be freed. If the attribute does not exist, #NULL is
 * returned.
 *
 * Return value: An #EVCardAttribute if found, or #NULL.
 **/
EVCardAttribute *
e_vcard_get_attribute (EVCard     *vcard,
		       const char *name)
{
        EVCardAttribute *attr;
        GList *attrs, *l;

        g_return_val_if_fail (E_IS_VCARD (vcard), NULL);
        g_return_val_if_fail (name != NULL, NULL);

        if (vcard->priv->vcard && !strcmp (name, EVC_UID)) {
                for (l = vcard->priv->attributes; l; l = l->next) {
                        if (strcmp ((attr = l->data)->name, name) == 0)
                                return attr;
                }
        }

        attrs = e_vcard_ensure_attributes (vcard);
        for (l = attrs; l; l = l->next) {
                if (strcmp ((attr = l->data)->name, name) == 0)
                        return attr;
        }

        return NULL;
}
/**
 * e_vcard_attribute_get_group:
 * @attr: an #EVCardAttribute
 *
 * Gets the group name of @attr.
 *
 * Return value: The attribute's group name.
 **/
const char*
e_vcard_attribute_get_group (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, NULL);

	return attr->group;
}

/**
 * e_vcard_attribute_get_name:
 * @attr: an #EVCardAttribute
 *
 * Gets the name of @attr.
 *
 * Return value: The attribute's name.
 **/
const char*
e_vcard_attribute_get_name (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, NULL);

	return attr->name;
}

/**
 * e_vcard_attribute_get_values:
 * @attr: an #EVCardAttribute
 *
 * Gets the list of values from @attr. The list and its
 * contents are owned by @attr, and must not be freed.
 * 
 * Return value: A list of string values.
 **/
GList*
e_vcard_attribute_get_values (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, NULL);

	return attr->values;
}

/**
 * e_vcard_attribute_get_values_decoded:
 * @attr: an #EVCardAttribute
 *
 * Gets the list of values from @attr, decoding them if
 * necessary. The list and its contents are owned by @attr,
 * and must not be freed.
 *
 * Return value: A list of values of type #GString.
 **/
GList*
e_vcard_attribute_get_values_decoded (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, NULL);

	if (!attr->decoded_values) {
		GList *l;
		switch (attr->encoding) {
		case EVC_ENCODING_RAW:
			for (l = attr->values; l; l = l->next)
				attr->decoded_values = g_list_prepend (attr->decoded_values, g_string_new ((char*)l->data));
			attr->decoded_values = g_list_reverse (attr->decoded_values);
			break;
		case EVC_ENCODING_BASE64:
			for (l = attr->values; l; l = l->next) {
				char *decoded = g_strdup ((char*)l->data);
				int len = _evc_base64_decode_simple (decoded, strlen (decoded));
				attr->decoded_values = g_list_prepend (attr->decoded_values, g_string_new_len (decoded, len));
				g_free (decoded);
			}
			attr->decoded_values = g_list_reverse (attr->decoded_values);
			break;
		case EVC_ENCODING_QP:
                        for (l = attr->values; l; l = l->next) {
                                char *decoded = _evc_quoted_printable_decode ((char *)l->data);
                                attr->decoded_values = g_list_prepend (attr->decoded_values, g_string_new (decoded));
                                g_free (decoded);
                        }
                        attr->decoded_values = g_list_reverse (attr->decoded_values);
			break;
		}
	}

	return attr->decoded_values;
}

/**
 * e_vcard_attribute_is_single_valued:
 * @attr: an #EVCardAttribute
 *
 * Checks if @attr has a single value.
 *
 * Return value: %TRUE if the attribute has exactly one value, %FALSE otherwise.
 **/
gboolean
e_vcard_attribute_is_single_valued (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, FALSE);

	if (attr->values == NULL
	    || attr->values->next != NULL)
		return FALSE;

	return TRUE;
}

/**
 * e_vcard_attribute_get_value:
 * @attr: an #EVCardAttribute
 *
 * Gets the value of a single-valued #EVCardAttribute, @attr.
 *
 * Return value: A newly allocated string representing the value.
 **/
char*
e_vcard_attribute_get_value (EVCardAttribute *attr)
{
	GList *values;

	g_return_val_if_fail (attr != NULL, NULL);

	values = attr->values;

	if (values && values->next)
		g_warning ("e_vcard_attribute_get_value called on multivalued attribute %s", attr->name);

	return values ? g_strdup ((char*)values->data) : NULL;
}

/**
 * e_vcard_attribute_get_value_decoded:
 * @attr: an #EVCardAttribute
 *
 * Gets the value of a single-valued #EVCardAttribute, @attr, decoding
 * it if necessary.
 *
 * Note: this function seems currently to be unused. Could be removed.
 *
 * Return value: A newly allocated #GString representing the value.
 **/
GString*
e_vcard_attribute_get_value_decoded (EVCardAttribute *attr)
{
	GList *values;
	GString *str = NULL;

	g_return_val_if_fail (attr != NULL, NULL);

	values = e_vcard_attribute_get_values_decoded (attr);

	if (!e_vcard_attribute_is_single_valued (attr))
		g_warning ("e_vcard_attribute_get_value_decoded called on multivalued attribute");

	if (values)
		str = values->data;

	return str ? g_string_new_len (str->str, str->len) : NULL;
}

/**
 * e_vcard_attribute_has_type:
 * @attr: an #EVCardAttribute
 * @typestr: a string representing the type
 *
 * Checks if @attr has an #EVCardAttributeParam of the specified type.
 *
 * Return value: %TRUE if such a parameter exists, %FALSE otherwise.
 **/
gboolean
e_vcard_attribute_has_type (EVCardAttribute *attr, const char *typestr)
{
	GList *p;

	g_return_val_if_fail (attr != NULL, FALSE);
	g_return_val_if_fail (typestr != NULL, FALSE);

	for (p = attr->params; p; p = p->next) {
		EVCardAttributeParam *param = p->data;

		if (!g_ascii_strcasecmp (e_vcard_attribute_param_get_name (param), EVC_TYPE)) {
			GList *values = e_vcard_attribute_param_get_values (param);
			GList *v;

			for (v = values; v; v = v->next) {
				if (!g_ascii_strcasecmp ((char*)v->data, typestr))
					return TRUE;
			}
		}
	}

	return FALSE;
}

/**
 * e_vcard_attribute_get_params:
 * @attr: an #EVCardAttribute
 * 
 * Gets the list of parameters from @attr. The list and its
 * contents are owned by @attr, and must not be freed.
 *
 * Return value: A list of elements of type #EVCardAttributeParam.
 **/
GList*
e_vcard_attribute_get_params (EVCardAttribute *attr)
{
	g_return_val_if_fail (attr != NULL, NULL);

	return attr->params;
}

/**
 * e_vcard_attribute_get_param:
 * @attr: an #EVCardAttribute
 * @name: a parameter name
 * 
 * Gets the list of values for the paramater @name from @attr. The list and its
 * contents are owned by @attr, and must not be freed.
 *
 * Return value: A list of string elements representing the parameter's values.
 **/
GList *
e_vcard_attribute_get_param (EVCardAttribute *attr, const char *name)
{
	GList *p;
	
	g_return_val_if_fail (attr != NULL, NULL);
	g_return_val_if_fail (name != NULL, NULL);
	
	for (p = attr->params; p; p = p->next) {
		EVCardAttributeParam *param = p->data;
		if (g_ascii_strcasecmp (e_vcard_attribute_param_get_name (param), name) == 0) {
			return e_vcard_attribute_param_get_values (param);
		}
	}

	return NULL;
}

/**
 * e_vcard_attribute_param_get_name:
 * @param: an #EVCardAttributeParam
 *
 * Gets the name of @param.
 *
 * Return value: The name of the parameter.
 **/
const char*
e_vcard_attribute_param_get_name (EVCardAttributeParam *param)
{
	g_return_val_if_fail (param != NULL, NULL);

	return param->name;
}

/**
 * e_vcard_attribute_param_get_values:
 * @param: an #EVCardAttributeParam
 * 
 * Gets the list of values from @param. The list and its
 * contents are owned by @param, and must not be freed.
 *
 * Return value: A list of string elements representing the parameter's values.
 **/
GList*
e_vcard_attribute_param_get_values (EVCardAttributeParam *param)
{
	g_return_val_if_fail (param != NULL, NULL);

	return param->values;
}



/* encoding/decoding stuff ripped from camel-mime-utils.c */

static char *_evc_base64_alphabet =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static unsigned char _evc_base64_rank[256];

static void
_evc_base64_init(void)
{
	int i;

	memset(_evc_base64_rank, 0xff, sizeof(_evc_base64_rank));
	for (i=0;i<64;i++) {
		_evc_base64_rank[(unsigned int)_evc_base64_alphabet[i]] = i;
	}
	_evc_base64_rank['='] = 0;
}

/* call this when finished encoding everything, to
   flush off the last little bit */
static size_t
_evc_base64_encode_close(unsigned char *in, size_t inlen, gboolean break_lines, unsigned char *out, int *state, int *save)
{
	int c1, c2;
	unsigned char *outptr = out;

	if (inlen>0)
		outptr += _evc_base64_encode_step(in, inlen, break_lines, outptr, state, save);

	c1 = ((unsigned char *)save)[1];
	c2 = ((unsigned char *)save)[2];
	
	d(printf("mode = %d\nc1 = %c\nc2 = %c\n",
		 (int)((char *)save)[0],
		 (int)((char *)save)[1],
		 (int)((char *)save)[2]));

	switch (((char *)save)[0]) {
	case 2:
		outptr[2] = _evc_base64_alphabet[ ( (c2 &0x0f) << 2 ) ];
		g_assert(outptr[2] != 0);
		goto skip;
	case 1:
		outptr[2] = '=';
	skip:
		outptr[0] = _evc_base64_alphabet[ c1 >> 2 ];
		outptr[1] = _evc_base64_alphabet[ c2 >> 4 | ( (c1&0x3) << 4 )];
		outptr[3] = '=';
		outptr += 4;
		break;
	}
	if (break_lines)
		*outptr++ = '\n';

	*save = 0;
	*state = 0;

	return outptr-out;
}

/*
  performs an 'encode step', only encodes blocks of 3 characters to the
  output at a time, saves left-over state in state and save (initialise to
  0 on first invocation).
*/
static size_t
_evc_base64_encode_step(unsigned char *in, size_t len, gboolean break_lines, unsigned char *out, int *state, int *save)
{
	register unsigned char *inptr, *outptr;

	if (len<=0)
		return 0;

	inptr = in;
	outptr = out;

	d(printf("we have %d chars, and %d saved chars\n", len, ((char *)save)[0]));

	if (len + ((char *)save)[0] > 2) {
		unsigned char *inend = in+len-2;
		register int c1, c2, c3;
		register int already;

		already = *state;

		switch (((char *)save)[0]) {
		case 1:	c1 = ((unsigned char *)save)[1]; goto skip1;
		case 2:	c1 = ((unsigned char *)save)[1];
			c2 = ((unsigned char *)save)[2]; goto skip2;
		}
		
		/* yes, we jump into the loop, no i'm not going to change it, it's beautiful! */
		while (inptr < inend) {
			c1 = *inptr++;
		skip1:
			c2 = *inptr++;
		skip2:
			c3 = *inptr++;
			*outptr++ = _evc_base64_alphabet[ c1 >> 2 ];
			*outptr++ = _evc_base64_alphabet[ c2 >> 4 | ( (c1&0x3) << 4 ) ];
			*outptr++ = _evc_base64_alphabet[ ( (c2 &0x0f) << 2 ) | (c3 >> 6) ];
			*outptr++ = _evc_base64_alphabet[ c3 & 0x3f ];
			/* this is a bit ugly ... */
			if (break_lines && (++already)>=19) {
				*outptr++='\n';
				already = 0;
			}
		}

		((char *)save)[0] = 0;
		len = 2-(inptr-inend);
		*state = already;
	}

	d(printf("state = %d, len = %d\n",
		 (int)((char *)save)[0],
		 len));

	if (len>0) {
		register char *saveout;

		/* points to the slot for the next char to save */
		saveout = & (((char *)save)[1]) + ((char *)save)[0];

		/* len can only be 0 1 or 2 */
		switch(len) {
		case 2:	*saveout++ = *inptr++;
		case 1:	*saveout++ = *inptr++;
		}
		((char *)save)[0]+=len;
	}

	d(printf("mode = %d\nc1 = %c\nc2 = %c\n",
		 (int)((char *)save)[0],
		 (int)((char *)save)[1],
		 (int)((char *)save)[2]));

	return outptr-out;
}


/**
 * base64_decode_step: decode a chunk of base64 encoded data
 * @in: input stream
 * @len: max length of data to decode
 * @out: output stream
 * @state: holds the number of bits that are stored in @save
 * @save: leftover bits that have not yet been decoded
 *
 * Decodes a chunk of base64 encoded data
 **/
static size_t
_evc_base64_decode_step(unsigned char *in, size_t len, unsigned char *out, int *state, unsigned int *save)
{
	register unsigned char *inptr, *outptr;
	unsigned char *inend, c;
	register unsigned int v;
	int i;

	inend = in+len;
	outptr = out;

	/* convert 4 base64 bytes to 3 normal bytes */
	v=*save;
	i=*state;
	inptr = in;
	while (inptr<inend) {
		c = _evc_base64_rank[*inptr++];
		if (c != 0xff) {
			v = (v<<6) | c;
			i++;
			if (i==4) {
				*outptr++ = v>>16;
				*outptr++ = v>>8;
				*outptr++ = v;
				i=0;
			}
		}
	}

	*save = v;
	*state = i;

	/* quick scan back for '=' on the end somewhere */
	/* fortunately we can drop 1 output char for each trailing = (upto 2) */
	i=2;
	while (inptr>in && i) {
		inptr--;
		if (_evc_base64_rank[*inptr] != 0xff) {
			if (*inptr == '=' && outptr>out)
				outptr--;
			i--;
		}
	}

	/* if i!= 0 then there is a truncation error! */
	return outptr-out;
}

static char *
_evc_base64_encode_simple (const char *data, size_t len)
{
	unsigned char *out;
	int state = 0, outlen;
	int save = 0;

	g_return_val_if_fail (data != NULL, NULL);

	if (len >= ((G_MAXSIZE - 1) / 4 - 1) * 3)
		return NULL;

	out = g_malloc ((len / 3 + 1) * 4 + 1);

	outlen = _evc_base64_encode_close ((unsigned char *)data, len, FALSE,
				      out, &state, &save);
	out[outlen] = 0;
	return (char *)out;
}

static size_t
_evc_base64_decode_simple (char *data, size_t len)
{
	int state = 0;
	unsigned int save = 0;

	g_return_val_if_fail (data != NULL, 0);

	return _evc_base64_decode_step ((unsigned char *)data, len,
					(unsigned char *)data, &state, &save);
}

/**
 * _evc_quoted_printable_decode: decodes quoted-printable text into
 * raw format.
 * @input: Input string in quoted-printable form.
 *
 * Decodes quoted-printable text into raw format. It removes soft line breaks
 * but doesn't handle escaped characters. #e_vcard_unescape_string should be
 * calld after this function if unescaping is needed.
 *
 * Return value: a newly allocated text in raw format
 */
static char *
_evc_quoted_printable_decode (const char *input)
{
        GString *output;

        output = g_string_new ("");

        for (; input && *input; input++) {
                /* special character follows */
                if (*input == '=') {
                        char a, b;

                        a = *(++input);
                        b = *(++input);

                        if (a == '\r' && b == '\n') {
                                /* skip soft line break */
                                continue;
                        }
                        else if (g_ascii_isxdigit(a) && g_ascii_isxdigit(b)) {
                                /* decode the hexadecimal value to unicode
                                 * character */
                                output = g_string_append_c (output, g_ascii_xdigit_value (a) * 16 +
                                                            g_ascii_xdigit_value (b));
                        }
                        else {
                                output = g_string_append_c (output, a);
                                output = g_string_append_c (output, b);
                        }
                }
                else {
                        output = g_string_append_c (output, *input);
                }
        }

        return g_string_free (output, FALSE);
}

/**
 * _evc_quoted_printable_encode: encodes a string into quoted-printable form.
 * @input: input string in raw format.
 * @size: size of the input buffer.
 * @is_qp: TRUE if the encoded string contains any special quoted-printabel
 * character, FALSE if the return value can be ignored because its the same as
 * @input.
 *
 * Encodes a raw string into quoted-printable form. It inserts soft line
 * breaks, but doesn't escapes special characters, if its needed then it
 * should be done before this function call with _evc_escape_string_21 function.
 *
 * Return value: a newly allocated string in quoted-printable format.
 */
static char *
_evc_quoted_printable_encode (const char *input, size_t size, gboolean *is_qp)
{
        GString *output;
        int l;

        output = g_string_new ("");
        *is_qp = FALSE;

        /* convert the char to quoted-printable sequence */
        for (l = 0; l < size; l++, input++) {
                unsigned char ch = *input;

                /* printable ASCII characters, SPACE and HTAB */
                if ( (ch > 31 && ch < 61) || (ch > 61 && ch < 127) || ch == 9 ) {
                        output = g_string_append_c (output, ch);
                }
                else {
                        g_string_append_printf (output, "=%02X", ch);
                        *is_qp = TRUE;
                }
        }

        return g_string_free (output, FALSE);
}

/**
 * _evc_escape_string_21:
 * @s: the string to escape
 *
 * Escapes a string according to vCard 2.1 specifications.
 *
 * Return value: A newly allocated, escaped string.
 **/
static char *
_evc_escape_string_21 (const char *s)
{
	GString *str;
	const char *p;

	str = g_string_new ("");

	/* Escape a string as described in vCard 2.1 specification */
	for (p = s; p && *p; p++) {
		switch (*p) {
		case ';':
			g_string_append (str, "\\;");
			break;
		case '\\':
			g_string_append (str, "\\\\");
			break;
		default:
			g_string_append_c (str, *p);
			break;
		}
	}

	return g_string_free (str, FALSE);
}

