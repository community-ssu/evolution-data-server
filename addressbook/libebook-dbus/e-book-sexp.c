/* -*- Mode: C; tab-width: 8; indent-tabs-mode: t; c-basic-offset: 8 -*- */
/*
 * e-book-sexp.c
 * Copyright 1999, 2000, 2001, Ximian, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License, version 2, as published by the Free Software Foundation.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#include "e-book-sexp.h"
#include "e-book-util.h"

#include "libedataserver/e-sexp.h"
#include "libedataserver/e-data-server-util.h"

#include <string.h>

static GObjectClass *parent_class;

typedef char * (* CompareFunc)   (const char *a, const char *b);
typedef char * (* NormalizeFunc) (const char *str);

typedef struct _SearchContext SearchContext;

struct _EBookSExpPrivate {
	ESExp *search_sexp;
	SearchContext *search_context;
};

struct _SearchContext {
	EContact *contact;
};

static char *
normalize_spaces (const char *str)
{
	if (str)
		return g_strstrip (g_strdup (str));

	return NULL;
}

static char *
normalize_phone_number (const char *str)
{
	if (str)
		return e_normalize_phone_number (str);

	return NULL;
}

static gboolean
compare_list (EContact *contact, const char *str, CompareFunc compare,
	      EContactField field, NormalizeFunc normalize)
{
	char     *needle = normalize (str);
	gboolean  rv = FALSE;
	GList    *values, *l;

	values = e_contact_get (contact, field);

	for (l = values; l != NULL; l = l->next) {
		char *value = normalize (l->data);

		if (value && compare (value, needle)) {
			g_free (value);
			rv = TRUE;
			break;
		}

		g_free (value);
	}

	g_list_foreach (values, (GFunc) g_free, NULL);
	g_list_free (values);

	return rv;
}

static gboolean
compare_im (EContact *contact, const char *str,
	    CompareFunc compare, EContactField field)
{
	return compare_list (contact, str, compare, field, normalize_spaces);
}

static gboolean
compare_im_aim (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_AIM);
}

static gboolean
compare_im_msn (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_MSN);
}

static gboolean
compare_im_icq (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_ICQ);
}

static gboolean
compare_im_yahoo (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_YAHOO);
}

static gboolean
compare_im_gadugadu (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_GADUGADU);
}


static gboolean
compare_im_jabber (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_JABBER);
}

static gboolean
compare_im_groupwise (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_GROUPWISE);
}

static gboolean
compare_im_skype (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_IM_SKYPE);
}

static gboolean
compare_email (EContact *contact, const char *str, CompareFunc compare)
{
	gboolean  rv = FALSE;
	char     *needle = normalize_spaces (str);
	int       i;

	for (i = E_CONTACT_EMAIL_1; i <= E_CONTACT_EMAIL_4; i ++) {
		char *email = normalize_spaces (e_contact_get_const (contact, i));

		if (email && compare(email, needle)) {
			g_free (email);
			rv = TRUE;
			break;
		}

		g_free (email);
	}

	g_free (needle);

	return rv;
}

static gboolean
compare_phone (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_list (contact, str, compare, E_CONTACT_TEL, normalize_phone_number);
}

static gboolean
compare_name (EContact *contact, const char *str, CompareFunc compare)
{
	EContactField fields[] = {
		E_CONTACT_FULL_NAME,
		E_CONTACT_FAMILY_NAME,
		E_CONTACT_GIVEN_NAME,
		E_CONTACT_NICKNAME,
	};

	gboolean  rv = FALSE;
	char     *needle = normalize_spaces (str);
	int       i;

	for (i = 0; i < G_N_ELEMENTS (fields); ++i) {
		char *name = normalize_spaces (e_contact_get_const (contact, fields[i]));

		if (name && compare (name, needle)) {
			g_free (name);
			rv = TRUE;
			break;
		}

		g_free (name);
	}

	g_free (needle);

	return rv;
}

static gboolean
compare_address (EContact *contact, const char *str, CompareFunc compare)
{
	char     *needle = normalize_spaces (str);
	gboolean  rv = FALSE;
	int       i;

	for (i = E_CONTACT_FIRST_ADDRESS_ID; i <= E_CONTACT_LAST_ADDRESS_ID; i ++) {
		EContactAddress *address = e_contact_get (contact, i);

		if (address) {
			const char *address_values[] = {
				address->po, address->street, address->ext,
				address->locality, address->region,
				address->code, address->country
			};

			for (i = 0; i < G_N_ELEMENTS (address_values); ++i) {
				char *value = normalize_spaces (address_values[i]);

				if (value && compare (value, needle)) {
					g_free (value);
					rv = TRUE;
					break;
				}

				g_free (value);
			}

			e_contact_address_free (address);

			if (rv)
				break;
		}
	}

	g_free (needle);

	return rv;

}

static gboolean
compare_category (EContact *contact, const char *str, CompareFunc compare)
{
	char *needle = normalize_spaces (str);
	gboolean rv = FALSE;
	GList *categories;
	GList *iterator;

	categories = e_contact_get (contact, E_CONTACT_CATEGORY_LIST);

	for (iterator = categories; iterator; iterator = iterator->next) {
		char *category = normalize_spaces (iterator->data);

		if (compare (category, needle)) {
			g_free (category);
			rv = TRUE;
			break;
		}

		g_free (category);
	}

	g_list_foreach (categories, (GFunc)g_free, NULL);
	g_list_free (categories);
	g_free (needle);

	return rv;
}

static gboolean
compare_sip (EContact *contact, const char *str, CompareFunc compare)
{
	return compare_im (contact, str, compare, E_CONTACT_SIP);
}

enum prop_type {
	PROP_TYPE_NORMAL,
	PROP_TYPE_LIST
};
static struct prop_info {
	EContactField field_id;
	const char *query_prop;
	enum prop_type prop_type;
	gboolean (*list_compare)(EContact *contact, const char *str, CompareFunc compare);

} prop_info_table[] = {
#define NORMAL_PROP(f,q) {f, q, PROP_TYPE_NORMAL, NULL}
#define LIST_PROP(q,c) {0, q, PROP_TYPE_LIST, c}

	/* query prop,   type,              list compare function */
	NORMAL_PROP ( E_CONTACT_FILE_AS, "file-as" ),
	NORMAL_PROP ( E_CONTACT_UID, "id" ),
	LIST_PROP ( "full-name", compare_name), /* not really a list, but we need to compare both full and surname */
	NORMAL_PROP ( E_CONTACT_HOMEPAGE_URL, "url"),
	NORMAL_PROP ( E_CONTACT_BLOG_URL, "blog-url"),
	NORMAL_PROP ( E_CONTACT_CALENDAR_URI, "calurl"),
	NORMAL_PROP ( E_CONTACT_FREEBUSY_URL, "fburl"),
	NORMAL_PROP ( E_CONTACT_ICS_CALENDAR, "icscalendar"),
	NORMAL_PROP ( E_CONTACT_VIDEO_URL, "video-url"),

	NORMAL_PROP ( E_CONTACT_MAILER, "mailer"),
	NORMAL_PROP ( E_CONTACT_ORG, "org"),
	NORMAL_PROP ( E_CONTACT_ORG_UNIT, "org-unit"),
	NORMAL_PROP ( E_CONTACT_OFFICE, "office"),
	NORMAL_PROP ( E_CONTACT_TITLE, "title"),
	NORMAL_PROP ( E_CONTACT_ROLE, "role"),
	NORMAL_PROP ( E_CONTACT_MANAGER, "manager"),
	NORMAL_PROP ( E_CONTACT_ASSISTANT, "assistant"),
	NORMAL_PROP ( E_CONTACT_NICKNAME, "nickname"),
	NORMAL_PROP ( E_CONTACT_SPOUSE, "spouse" ),
	NORMAL_PROP ( E_CONTACT_NOTE, "note"),
	LIST_PROP ( "im-aim",    compare_im_aim ),
	LIST_PROP ( "im-msn",    compare_im_msn ),
	LIST_PROP ( "im-icq",    compare_im_icq ),
	LIST_PROP ( "im-jabber", compare_im_jabber ),
	LIST_PROP ( "im-yahoo",  compare_im_yahoo ),
	LIST_PROP ( "im-gadugadu",  compare_im_gadugadu ),
	LIST_PROP ( "im-groupwise", compare_im_groupwise ),
	LIST_PROP ( "im-skype", compare_im_skype ),
	LIST_PROP ( "email",     compare_email ),
	LIST_PROP ( "phone",     compare_phone ),
	LIST_PROP ( "address",   compare_address ),
	LIST_PROP ( "category-list",  compare_category ),
	LIST_PROP ( "sip",  compare_sip ),
};

static ESExpResult *
entry_compare(SearchContext *ctx, struct _ESExp *f,
	      int argc, struct _ESExpResult **argv,
	      CompareFunc compare)
{
	ESExpResult *r;
	int truth = FALSE;

	if (argc == 2
	    && argv[0]->type == ESEXP_RES_STRING
	    && argv[1]->type == ESEXP_RES_STRING) {
		char *propname;
		struct prop_info *info = NULL;
		int i;
		gboolean any_field;

		propname = argv[0]->value.string;

		any_field = !strcmp(propname, "x-evolution-any-field");
		for (i = 0; i < G_N_ELEMENTS (prop_info_table); i ++) {
			if (any_field
			    || !strcmp (prop_info_table[i].query_prop, propname)) {
				info = &prop_info_table[i];

				if (any_field && info->field_id == E_CONTACT_UID) {
					/* We need to skip UID from any field contains search
					 * any-field search should be supported for the
					 * visible fields only.
					 */
					truth = FALSE;
				}
				else if (info->prop_type == PROP_TYPE_NORMAL) {
					char *needle, *prop;
					/* straight string property matches */

					needle = normalize_spaces (argv[1]->value.string);
					prop = normalize_spaces (e_contact_get_const (ctx->contact, info->field_id));

					if (compare (prop ? prop : "", needle))
						truth = TRUE;

					g_free (needle);
					g_free (prop);
				}
				else if (info->prop_type == PROP_TYPE_LIST) {
					/* the special searches that match any of the list elements */
					truth = info->list_compare (ctx->contact, argv[1]->value.string, compare);
				}

				/* if we're looking at all fields and find a match,
				   or if we're just looking at this one field,
				   break. */
				if ((any_field && truth)
				    || !any_field)
					break;
			}
		}

	}
	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	r->value.bool = truth;

	return r;
}

static ESExpResult *
vcard_compare (SearchContext *ctx, struct _ESExp *f,
	       int argc, struct _ESExpResult **argv,
	       CompareFunc compare)
{
	ESExpResult *r;
	int truth = FALSE;

	if (argc == 2 &&
	    argv[0]->type == ESEXP_RES_STRING &&
	    argv[1]->type == ESEXP_RES_STRING) {
		NormalizeFunc normalize = normalize_spaces;
		const char *field;
		char *needle;
		GList *attrs;

		field = argv[0]->value.string;

		if (!g_ascii_strcasecmp (field, EVC_TEL))
			normalize = normalize_phone_number;

		needle = normalize (argv[1]->value.string);

		for (attrs = e_vcard_get_attributes (E_VCARD (ctx->contact)); attrs; attrs = attrs->next) {
			EVCardAttribute *attr = attrs->data;
			GList *values;
			if (g_ascii_strcasecmp (e_vcard_attribute_get_name (attr), field) != 0)
				continue;

			values = e_vcard_attribute_get_values (attr);
			for (; values; values = values->next) {
				char *value = normalize (values->data);

				if (compare (value, needle)) {
					g_free (value);
					truth = TRUE;
					break;
				}

				g_free (value);
			}
		}

		g_free (needle);
	}
	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	r->value.bool = truth;

	return r;
}

static ESExpResult *
func_contains(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return entry_compare (ctx, f, argc, argv, (CompareFunc) e_util_utf8_strstrcase);
}

static ESExpResult *
func_contains_vcard(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return vcard_compare (ctx, f, argc, argv, (CompareFunc) e_util_utf8_strstrcase);
}

static char *
is_helper (const char *s1, const char *s2)
{
	if (!e_util_utf8_strcasecmp (s1, s2))
		return (char*)s1;
	else
		return NULL;
}

static ESExpResult *
func_is(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return entry_compare (ctx, f, argc, argv, is_helper);
}

static ESExpResult *
func_is_vcard(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return vcard_compare (ctx, f, argc, argv, is_helper);
}

static char *
endswith_helper (const char *s1, const char *s2)
{
	char *p;

	while((p = (char*) e_util_utf8_strstrcase(s1, s2))) {
		if ((strlen(p) == strlen(s2)))
			return p;
		else
			s1 = g_utf8_next_char (p);
	}
	return NULL;
}

static ESExpResult *
func_endswith(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return entry_compare (ctx, f, argc, argv, endswith_helper);
}

static ESExpResult *
func_endswith_vcard(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return vcard_compare (ctx, f, argc, argv, endswith_helper);
}

static char *
beginswith_helper (const char *s1, const char *s2)
{
	char *p;
	if ((p = (char*) e_util_utf8_strstrcase(s1, s2))
	    && (p == s1))
		return p;
	else
		return NULL;
}

static ESExpResult *
func_beginswith(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return entry_compare (ctx, f, argc, argv, beginswith_helper);
}

static ESExpResult *
func_beginswith_vcard(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;

	return vcard_compare (ctx, f, argc, argv, beginswith_helper);
}

static ESExpResult *
func_exists(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;
	ESExpResult *r;
	int truth = FALSE;

	if (argc == 1
	    && argv[0]->type == ESEXP_RES_STRING) {
		char *propname;
		struct prop_info *info = NULL;
		int i;

		propname = argv[0]->value.string;

		for (i = 0; i < G_N_ELEMENTS (prop_info_table); i ++) {
			if (!strcmp (prop_info_table[i].query_prop, propname)) {
				info = &prop_info_table[i];

				if (info->prop_type == PROP_TYPE_NORMAL) {
					const char *prop = NULL;
					/* searches where the query's property
					   maps directly to an ecard property */

					prop = e_contact_get_const (ctx->contact, info->field_id);

					if (prop && *prop)
						truth = TRUE;
				}
				else if (info->prop_type == PROP_TYPE_LIST) {
				/* the special searches that match any of the list elements */
					truth = info->list_compare (ctx->contact, "", (char *(*)(const char*, const char*)) e_util_utf8_strstrcase);
				}

				break;
			}
		}

	}
	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	r->value.bool = truth;

	return r;
}

static ESExpResult *
func_exists_vcard(struct _ESExp *f, int argc, struct _ESExpResult **argv, void *data)
{
	SearchContext *ctx = data;
	ESExpResult *r;
	int truth = FALSE;

	if (argc == 1 && argv[0]->type == ESEXP_RES_STRING) {
		const char *attr_name;
		EVCardAttribute *attr;
		GList *values;
		char *s;

		attr_name = argv[0]->value.string;
		attr = e_vcard_get_attribute (E_VCARD (ctx->contact), attr_name);
		if (attr) {
			values = e_vcard_attribute_get_values (attr);
			if (g_list_length (values) > 0) {
				s = values->data;
				if (s[0] != '\0') {
					truth = TRUE;
				}
			}
		}
	}

	r = e_sexp_result_new(f, ESEXP_RES_BOOL);
	r->value.bool = truth;

	return r;
}

/* 'builtin' functions */
static struct {
	char *name;
	ESExpFunc *func;
	int type;		/* set to 1 if a function can perform shortcut evaluation, or
				   doesn't execute everything, 0 otherwise */
} symbols[] = {
	{ "contains", func_contains, 0 },
	{ "contains_vcard", func_contains_vcard, 0 },
	{ "is", func_is, 0 },
	{ "is_vcard", func_is_vcard, 0 },
	{ "beginswith", func_beginswith, 0 },
	{ "beginswith_vcard", func_beginswith_vcard, 0 },
	{ "endswith", func_endswith, 0 },
	{ "endswith_vcard", func_endswith_vcard, 0 },
	{ "exists", func_exists, 0 },
	{ "exists_vcard", func_exists_vcard, 0 },
};

/**
 * e_book_sexp_match_contact:
 * @sexp: an #EBookSExp
 * @contact: an #EContact
 *
 * Checks if @contact matches @sexp.
 *
 * Return value: %TRUE if the contact matches, %FALSE otherwise.
 **/
gboolean
e_book_sexp_match_contact (EBookSExp *sexp, EContact *contact)
{
	ESExpResult *r;
	gboolean retval;

	if (!contact) {
		g_warning ("null EContact passed to e_book_sexp_match_contact");
		return FALSE;
	}

	sexp->priv->search_context->contact = g_object_ref (contact);

	r = e_sexp_eval(sexp->priv->search_sexp);

	retval = (r && r->type == ESEXP_RES_BOOL && r->value.bool);

	g_object_unref(sexp->priv->search_context->contact);

	e_sexp_result_free(sexp->priv->search_sexp, r);

	return retval;
}

/**
 * e_book_sexp_match_vcard:
 * @sexp: an #EBookSExp
 * @vcard: a VCard string
 *
 * Checks if @vcard matches @sexp.
 *
 * Return value: %TRUE if the VCard matches, %FALSE otherwise.
 **/
gboolean
e_book_sexp_match_vcard (EBookSExp *sexp, const char *vcard)
{
	EContact *contact;
	gboolean retval;

	contact = e_contact_new_from_vcard (vcard);

	retval = e_book_sexp_match_contact (sexp, contact);

	g_object_unref(contact);

	return retval;
}



/**
 * e_book_sexp_new:
 * @text: an s-expression to parse
 *
 * Creates a new #EBookSExp from @text.
 *
 * Return value: A new #EBookSExp.
 **/
EBookSExp *
e_book_sexp_new (const char *text)
{
	EBookSExp *sexp = g_object_new (E_TYPE_BOOK_SEXP, NULL);

	if (!e_book_sexp_parse (sexp, text)) {
		g_object_unref (sexp);
		sexp = NULL;
	}

	return sexp;
}

gboolean
e_book_sexp_parse (EBookSExp  *sexp,
		   const char *text)
{
	int esexp_error;
	int i;

	if (sexp->priv->search_sexp)
		e_sexp_unref (sexp->priv->search_sexp);

	sexp->priv->search_sexp = e_sexp_new();

	for(i = 0; i < G_N_ELEMENTS (symbols); ++i) {
		if (symbols[i].type == 1) {
			e_sexp_add_ifunction(sexp->priv->search_sexp, 0, symbols[i].name,
					     (ESExpIFunc *)symbols[i].func, sexp->priv->search_context);
		}
		else {
			e_sexp_add_function(sexp->priv->search_sexp, 0, symbols[i].name,
					    symbols[i].func, sexp->priv->search_context);
		}
	}

	e_sexp_input_text(sexp->priv->search_sexp, text, strlen(text));
	esexp_error = e_sexp_parse(sexp->priv->search_sexp);

	return (esexp_error != -1);
}

static void
e_book_sexp_dispose (GObject *object)
{
	EBookSExp *sexp = E_BOOK_SEXP (object);

	if (sexp->priv) {
		e_sexp_unref (sexp->priv->search_sexp);
		g_free (sexp->priv->search_context);
		g_free (sexp->priv);
		sexp->priv = NULL;
	}

	if (G_OBJECT_CLASS (parent_class)->dispose)
		G_OBJECT_CLASS (parent_class)->dispose (object);
}

static void
e_book_sexp_class_init (EBookSExpClass *klass)
{
	GObjectClass  *object_class = G_OBJECT_CLASS (klass);

	parent_class = g_type_class_peek_parent (klass);

	/* Set the virtual methods. */

	object_class->dispose = e_book_sexp_dispose;
}

static void
e_book_sexp_init (EBookSExp *sexp)
{
	EBookSExpPrivate *priv;

	priv             = g_new0 (EBookSExpPrivate, 1);

	sexp->priv = priv;
	priv->search_context = g_new (SearchContext, 1);
}

/**
 * e_book_sexp_get_type:
 */
GType
e_book_sexp_get_type (void)
{
	static GType type = 0;

	if (! type) {
		GTypeInfo info = {
			sizeof (EBookSExpClass),
			NULL, /* base_class_init */
			NULL, /* base_class_finalize */
			(GClassInitFunc)  e_book_sexp_class_init,
			NULL, /* class_finalize */
			NULL, /* class_data */
			sizeof (EBookSExp),
			0,    /* n_preallocs */
			(GInstanceInitFunc) e_book_sexp_init
		};

		type = g_type_register_static (G_TYPE_OBJECT, "EBookSExp", &info, 0);
	}

	return type;
}
