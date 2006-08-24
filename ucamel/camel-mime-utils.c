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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/param.h>  /* for MAXHOSTNAMELEN */
#include <sys/stat.h>
#include <pthread.h>
#include <unistd.h>
#include <regex.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>

#ifndef MAXHOSTNAMELEN
#define MAXHOSTNAMELEN 1024
#endif

#include <glib.h>
#include <libedataserver/e-iconv.h>
#include <libedataserver/e-time-utils.h>

#include "camel-mime-utils.h"
#include "camel-charset-map.h"
//#include "camel-net-utils.h"

/* for all non-essential warnings ... */
#define w(x)

#define d(x)
#define d2(x)

static const char base64_alphabet[] =
"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static const unsigned char tohex[16] = {
	'0', '1', '2', '3', '4', '5', '6', '7',
	'8', '9', 'A', 'B', 'C', 'D', 'E', 'F'
};

static const unsigned char camel_mime_base64_rank[256] = {
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0x3e, 0xff, 0xff, 0xff, 0x3f,
	0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a, 0x3b,
	0x3c, 0x3d, 0xff, 0xff, 0xff, 0x00, 0xff, 0xff,
	0xff, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
	0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
	0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
	0x17, 0x18, 0x19, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f, 0x20,
	0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x28,
	0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30,
	0x31, 0x32, 0x33, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
};

static char *camel_header_decode_string (const char *in, const char *default_charset);
static void camel_header_address_add_member (struct _camel_header_address *, struct _camel_header_address *member);
static void camel_header_address_list_append_list (struct _camel_header_address **l, struct _camel_header_address **h);
static void camel_header_address_list_append (struct _camel_header_address **, struct _camel_header_address *);
static char *camel_header_unfold (const char *in);
static size_t camel_base64_decode_step (unsigned char *in, size_t len, unsigned char *out, int *state, unsigned int *save);

/**
 * camel_base64_decode_step: decode a chunk of base64 encoded data
 * @in: input stream
 * @len: max length of data to decode
 * @out: output stream
 * @state: holds the number of bits that are stored in @save
 * @save: leftover bits that have not yet been decoded
 *
 * Decodes a chunk of base64 encoded data
 **/
static size_t
camel_base64_decode_step(unsigned char *in, size_t len, unsigned char *out, int *state, unsigned int *save)
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
		c = camel_mime_base64_rank[*inptr++];
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
		if (camel_mime_base64_rank[*inptr] != 0xff) {
			if (*inptr == '=' && outptr>out)
				outptr--;
			i--;
		}
	}

	/* if i!= 0 then there is a truncation error! */
	return outptr-out;
}


/*
  this is for the "Q" encoding of international words,
  which is slightly different than plain quoted-printable (mainly by allowing 0x20 <> _)
*/
static size_t
quoted_decode(const unsigned char *in, size_t len, unsigned char *out)
{
	register const unsigned char *inptr;
	register unsigned char *outptr;
	unsigned const char *inend;
	unsigned char c, c1;
	int ret = 0;

	inend = in+len;
	outptr = out;

	d(printf("decoding text '%.*s'\n", len, in));

	inptr = in;
	while (inptr<inend) {
		c = *inptr++;
		if (c=='=') {
			/* silently ignore truncated data? */
			if (inend-in>=2) {
				c = toupper(*inptr++);
				c1 = toupper(*inptr++);
				*outptr++ = (((c>='A'?c-'A'+10:c-'0')&0x0f) << 4)
					| ((c1>='A'?c1-'A'+10:c1-'0')&0x0f);
			} else {
				ret = -1;
				break;
			}
		} else if (c=='_') {
			*outptr++ = 0x20;
		} else if (c==' ' || c==0x09) {
			/* FIXME: this is an error! ignore for now ... */
			ret = -1;
			break;
		} else {
			*outptr++ = c;
		}
	}
	if (ret==0) {
		return outptr-out;
	}
	return 0;
}

/* rfc2047 version of quoted-printable */
/* safemask is the mask to apply to the camel_mime_special_table to determine what
   characters can safely be included without encoding */
static size_t
quoted_encode (const unsigned char *in, size_t len, unsigned char *out, unsigned short safemask)
{
	register const unsigned char *inptr, *inend;
	unsigned char *outptr;
	unsigned char c;

	inptr = in;
	inend = in + len;
	outptr = out;
	while (inptr < inend) {
		c = *inptr++;
		if (c==' ') {
			*outptr++ = '_';
		} else if (camel_mime_special_table[c] & safemask) {
			*outptr++ = c;
		} else {
			*outptr++ = '=';
			*outptr++ = tohex[(c >> 4) & 0xf];
			*outptr++ = tohex[c & 0xf];
		}
	}

	d(printf("encoding '%.*s' = '%.*s'\n", len, in, outptr-out, out));

	return (outptr - out);
}


static void
header_decode_lwsp(const char **in)
{
	const char *inptr = *in;
	char c;

	d2(printf("is ws: '%s'\n", *in));

	while (camel_mime_is_lwsp(*inptr) || (*inptr =='(' && *inptr != '\0')) {
		while (camel_mime_is_lwsp(*inptr) && inptr != '\0') {
			d2(printf("(%c)", *inptr));
			inptr++;
		}
		d2(printf("\n"));

		/* check for comments */
		if (*inptr == '(') {
			int depth = 1;
			inptr++;
			while (depth && (c=*inptr) && *inptr != '\0') {
				if (c=='\\' && inptr[1]) {
					inptr++;
				} else if (c=='(') {
					depth++;
				} else if (c==')') {
					depth--;
				}
				inptr++;
			}
		}
	}
	*in = inptr;
}

/* decode rfc 2047 encoded string segment */
static char *
rfc2047_decode_word(const char *in, size_t len)
{
	const char *inptr = in+2;
	const char *inend = in+len-2;
	const char *inbuf;
	const char *charset;
	char *encname, *p;
	int tmplen;
	size_t ret;
	char *decword = NULL;
	char *decoded = NULL;
	char *outbase = NULL;
	char *outbuf;
	size_t inlen, outlen;
	gboolean retried = FALSE;
	iconv_t ic;
	
	d(printf("rfc2047: decoding '%.*s'\n", len, in));

	/* quick check to see if this could possibly be a real encoded word */
	if (len < 8 || !(in[0] == '=' && in[1] == '?' && in[len-1] == '=' && in[len-2] == '?')) {
		d(printf("invalid\n"));
		return NULL;
	}
	
	/* skip past the charset to the encoding type */
	inptr = memchr (inptr, '?', inend-inptr);
	if (inptr != NULL && inptr < inend + 2 && inptr[2] == '?') {
		d(printf("found ?, encoding is '%c'\n", inptr[0]));
		inptr++;
		tmplen = inend-inptr-2;
		decword = g_alloca (tmplen); /* this will always be more-than-enough room */
		switch(toupper(inptr[0])) {
		case 'Q':
			inlen = quoted_decode(inptr+2, tmplen, decword);
			break;
		case 'B': {
			int state = 0;
			unsigned int save = 0;
			
			inlen = camel_base64_decode_step((char *)inptr+2, tmplen, decword, &state, &save);
			/* if state != 0 then error? */
			break;
		}
		default:
			/* uhhh, unknown encoding type - probably an invalid encoded word string */
			return NULL;
		}
		d(printf("The encoded length = %d\n", inlen));
		if (inlen > 0) {
			/* yuck, all this snot is to setup iconv! */
			tmplen = inptr - in - 3;
			encname = g_alloca (tmplen + 1);
			memcpy (encname, in + 2, tmplen);
			encname[tmplen] = '\0';
			
			/* rfc2231 updates rfc2047 encoded words...
			 * The ABNF given in RFC 2047 for encoded-words is:
			 *   encoded-word := "=?" charset "?" encoding "?" encoded-text "?="
			 * This specification changes this ABNF to:
			 *   encoded-word := "=?" charset ["*" language] "?" encoding "?" encoded-text "?="
			 */
			
			/* trim off the 'language' part if it's there... */
			p = strchr (encname, '*');
			if (p)
				*p = '\0';
			
			charset = e_iconv_charset_name (encname);
			
			inbuf = decword;
			
			outlen = inlen * 6 + 16;
			outbase = g_alloca (outlen);
			outbuf = outbase;
			
		retry:
			ic = e_iconv_open ("UTF-8", charset);
			if (ic != (iconv_t) -1) {
				ret = e_iconv (ic, &inbuf, &inlen, &outbuf, &outlen);
				if (ret != (size_t) -1) {
					e_iconv (ic, NULL, 0, &outbuf, &outlen);
					*outbuf = 0;
					decoded = g_strdup (outbase);
				}
				e_iconv_close (ic);
			} else {
				w(g_warning ("Cannot decode charset, header display may be corrupt: %s: %s",
					     charset, strerror (errno)));
				
				if (!retried) {
					charset = e_iconv_locale_charset ();
					if (!charset)
						charset = "iso-8859-1";
					
					retried = TRUE;
					goto retry;
				}
				
				/* we return the encoded word here because we've got to return valid utf8 */
				decoded = g_strndup (in, inlen);
			}
		}
	}
	
	d(printf("decoded '%s'\n", decoded));
	
	return decoded;
}

/* ok, a lot of mailers are BROKEN, and send iso-latin1 encoded
   headers, when they should just be sticking to US-ASCII
   according to the rfc's.  Anyway, since the conversion to utf-8
   is trivial, just do it here without iconv */
static GString *
append_latin1 (GString *out, const char *in, size_t len)
{
	unsigned int c;
	
	while (len) {
		c = (unsigned int)*in++;
		len--;
		if (c & 0x80) {
			out = g_string_append_c (out, 0xc0 | ((c >> 6) & 0x3));  /* 110000xx */
			out = g_string_append_c (out, 0x80 | (c & 0x3f));        /* 10xxxxxx */
		} else {
			out = g_string_append_c (out, c);
		}
	}
	return out;
}

static int
append_8bit (GString *out, const char *inbuf, size_t inlen, const char *charset)
{
	char *outbase, *outbuf;
	size_t outlen;
	iconv_t ic;
	
	ic = e_iconv_open ("UTF-8", charset);
	if (ic == (iconv_t) -1)
		return FALSE;

	outlen = inlen * 6 + 16;
	outbuf = outbase = g_malloc(outlen);
	
	if (e_iconv (ic, &inbuf, &inlen, &outbuf, &outlen) == (size_t) -1) {
		w(g_warning("Conversion to '%s' failed: %s", charset, strerror (errno)));
		g_free(outbase);
		e_iconv_close (ic);
		return FALSE;
	}
	
	e_iconv (ic, NULL, NULL, &outbuf, &outlen);
	
	*outbuf = 0;
	g_string_append(out, outbase);
	g_free(outbase);
	e_iconv_close (ic);

	return TRUE;
	
}

static GString *
append_quoted_pair (GString *str, const char *in, gssize inlen)
{
	register const char *inptr = in;
	const char *inend = in + inlen;
	char c;
	
	while (inptr < inend) {
		c = *inptr++;
		if (c == '\\' && inptr < inend)
			g_string_append_c (str, *inptr++);
		else
			g_string_append_c (str, c);
	}

	return str;
}

/* decodes a simple text, rfc822 + rfc2047 */
static char *
header_decode_text (const char *in, size_t inlen, int ctext, const char *default_charset)
{
	GString *out;
	const char *inptr, *inend, *start, *chunk, *locale_charset;
	GString *(* append) (GString *, const char *, gssize);
	char *dword = NULL;
	guint32 mask;
	
	locale_charset = e_iconv_locale_charset ();
	
	if (ctext) {
		mask = (CAMEL_MIME_IS_SPECIAL | CAMEL_MIME_IS_SPACE | CAMEL_MIME_IS_CTRL);
		append = append_quoted_pair;
	} else {
		mask = (CAMEL_MIME_IS_LWSP);
		append = g_string_append_len;
	}
	
	out = g_string_new ("");
	inptr = in;
	inend = inptr + inlen;
	chunk = NULL;

	while (inptr < inend) {
		start = inptr;
		while (inptr < inend && camel_mime_is_type (*inptr, mask))
			inptr++;

		if (inptr == inend) {
			append (out, start, inptr - start);
			break;
		} else if (dword == NULL) {
			append (out, start, inptr - start);
		} else {
			chunk = start;
		}

		start = inptr;
		while (inptr < inend && !camel_mime_is_type (*inptr, mask))
			inptr++;

		dword = rfc2047_decode_word(start, inptr-start);
		if (dword) {
			g_string_append(out, dword);
			g_free(dword);
		} else {
			if (!chunk)
				chunk = start;
			
			if ((default_charset == NULL || !append_8bit (out, chunk, inptr-chunk, default_charset))
			    && (locale_charset == NULL || !append_8bit(out, chunk, inptr-chunk, locale_charset)))
				append_latin1(out, chunk, inptr-chunk);
		}
		
		chunk = NULL;
	}

	dword = out->str;
	g_string_free (out, FALSE);
	
	return dword;
}

static char *
camel_header_decode_string (const char *in, const char *default_charset)
{
	if (in == NULL)
		return NULL;
	return header_decode_text (in, strlen (in), FALSE, default_charset);
}

/* how long a sequence of pre-encoded words should be less than, to attempt to 
   fit into a properly folded word.  Only a guide. */
#define CAMEL_FOLD_PREENCODED (24)

/* FIXME: needs a way to cache iconv opens for different charsets? */
static void
rfc2047_encode_word(GString *outstring, const char *in, size_t len, const char *type, unsigned short safemask)
{
	iconv_t ic = (iconv_t) -1;
	char *buffer, *out, *ascii;
	size_t inlen, outlen, enclen, bufflen;
	const char *inptr, *p;
	int first = 1;

	d(printf("Converting [%d] '%.*s' to %s\n", len, len, in, type));

	/* convert utf8->encoding */
	bufflen = len * 6 + 16;
	buffer = g_alloca (bufflen);
	inlen = len;
	inptr = in;
	
	ascii = g_alloca (bufflen);
	
	if (strcasecmp (type, "UTF-8") != 0)
		ic = e_iconv_open (type, "UTF-8");
	
	while (inlen) {
		size_t convlen, proclen;
		int i;
		
		/* break up words into smaller bits, what we really want is encoded + overhead < 75,
		   but we'll just guess what that means in terms of input chars, and assume its good enough */

		out = buffer;
		outlen = bufflen;

		if (ic == (iconv_t) -1) {
			/* native encoding case, the easy one (?) */
			/* we work out how much we can convert, and still be in length */
			/* proclen will be the result of input characters that we can convert, to the nearest
			   (approximated) valid utf8 char */
			convlen = 0;
			proclen = 0;
			p = inptr;
			i = 0;
			while (p < (in+len) && convlen < (75 - strlen("=?utf-8?q\?\?="))) {
				unsigned char c = *p++;

				if (c >= 0xc0)
					proclen = i;
				i++;
				if (c < 0x80)
					proclen = i;
				if (camel_mime_special_table[c] & safemask)
					convlen += 1;
				else
					convlen += 3;
			}
			/* well, we probably have broken utf8, just copy it anyway what the heck */
			if (proclen == 0) {
				w(g_warning("Appear to have truncated utf8 sequence"));
				proclen = inlen;
			}
			memcpy(out, inptr, proclen);
			inptr += proclen;
			inlen -= proclen;
			out += proclen;
		} else {
			/* well we could do similar, but we can't (without undue effort), we'll just break it up into
			   hopefully-small-enough chunks, and leave it at that */
			convlen = MIN(inlen, CAMEL_FOLD_PREENCODED);
			p = inptr;
			if (e_iconv (ic, &inptr, &convlen, &out, &outlen) == (size_t) -1 && errno != EINVAL) {
				w(g_warning("Conversion problem: conversion truncated: %s", strerror (errno)));
				/* blah, we include it anyway, better than infinite loop ... */
				inptr += convlen;
			} else {
				/* make sure we flush out any shift state */
				e_iconv (ic, NULL, 0, &out, &outlen);
			}
			inlen -= (inptr - p);
		}
		
		enclen = out-buffer;
		
		if (enclen) {
			/* create token */
			out = ascii;
			if (first)
				first = 0;
			else
				*out++ = ' ';
			out += sprintf (out, "=?%s?Q?", type);
			out += quoted_encode (buffer, enclen, out, safemask);
			sprintf (out, "?=");
			
			d(printf("converted part = %s\n", ascii));
			
			g_string_append (outstring, ascii);
		}
	}
	
	if (ic != (iconv_t) -1)
		e_iconv_close (ic);
}

/* apply quoted-string rules to a string */
static void
quote_word(GString *out, gboolean do_quotes, const char *start, size_t len)
{
	int i, c;

	/* TODO: What about folding on long lines? */
	if (do_quotes)
		g_string_append_c(out, '"');
	for (i=0;i<len;i++) {
		c = *start++;
		if (c == '\"' || c=='\\' || c=='\r')
			g_string_append_c(out, '\\');
		g_string_append_c(out, c);
	}
	if (do_quotes)
		g_string_append_c(out, '"');
}

/* incrementing possibility for the word type */
enum _phrase_word_t {
	WORD_ATOM,
	WORD_QSTRING,
	WORD_2047
};

struct _phrase_word {
	const unsigned char *start, *end;
	enum _phrase_word_t type;
	int encoding;
};

static gboolean
word_types_compatable (enum _phrase_word_t type1, enum _phrase_word_t type2)
{
	switch (type1) {
	case WORD_ATOM:
		return type2 == WORD_QSTRING;
	case WORD_QSTRING:
		return type2 != WORD_2047;
	case WORD_2047:
		return type2 == WORD_2047;
	default:
		return FALSE;
	}
}

/* split the input into words with info about each word
 * merge common word types clean up */
static GList *
header_encode_phrase_get_words (const unsigned char *in)
{
	const unsigned char *inptr = in, *start, *last;
	struct _phrase_word *word;
	enum _phrase_word_t type;
	int encoding, count = 0;
	GList *words = NULL;
	
	/* break the input into words */
	type = WORD_ATOM;
	last = inptr;
	start = inptr;
	encoding = 0;
	while (inptr && *inptr) {
		gunichar c;
		const char *newinptr;
		
		newinptr = g_utf8_next_char (inptr);
		c = g_utf8_get_char (inptr);
		
		if (!g_unichar_validate (c)) {
			w(g_warning ("Invalid UTF-8 sequence encountered (pos %d, char '%c'): %s",
				     (inptr - in), inptr[0], in));
			inptr++;
			continue;
		}
		
		inptr = newinptr;
		if (g_unichar_isspace (c)) {
			if (count > 0) {
				word = g_new0 (struct _phrase_word, 1);
				word->start = start;
				word->end = last;
				word->type = type;
				word->encoding = encoding;
				words = g_list_append (words, word);
				count = 0;
			}
			
			start = inptr;
			type = WORD_ATOM;
			encoding = 0;
		} else {
			count++;
			if (c < 128) {
				if (!camel_mime_is_atom (c))
					type = MAX (type, WORD_QSTRING);
			} else if (c > 127 && c < 256) {
				type = WORD_2047;
				encoding = MAX (encoding, 1);
			} else if (c >= 256) {
				type = WORD_2047;
				encoding = MAX (encoding, 2);
			}
		}
		
		last = inptr;
	}
	
	if (count > 0) {
		word = g_new0 (struct _phrase_word, 1);
		word->start = start;
		word->end = last;
		word->type = type;
		word->encoding = encoding;
		words = g_list_append (words, word);
	}
	
	return words;
}

#define MERGED_WORD_LT_FOLDLEN(wordlen, type) ((type) == WORD_2047 ? (wordlen) < CAMEL_FOLD_PREENCODED : (wordlen) < (CAMEL_FOLD_SIZE - 8))

static gboolean
header_encode_phrase_merge_words (GList **wordsp)
{
	GList *wordl, *nextl, *words = *wordsp;
	struct _phrase_word *word, *next;
	gboolean merged = FALSE;
	
	/* scan the list, checking for words of similar types that can be merged */
	wordl = words;
	while (wordl) {
		word = wordl->data;
		nextl = g_list_next (wordl);
		
		while (nextl) {
			next = nextl->data;
			/* merge nodes of the same type AND we are not creating too long a string */
			if (word_types_compatable (word->type, next->type)) {
				if (MERGED_WORD_LT_FOLDLEN (next->end - word->start, MAX (word->type, next->type))) {
					/* the resulting word type is the MAX of the 2 types */
					word->type = MAX(word->type, next->type);
					word->encoding = MAX(word->encoding, next->encoding);
					word->end = next->end;
					words = g_list_remove_link (words, nextl);
					g_list_free_1 (nextl);
					g_free (next);
					
					nextl = g_list_next (wordl);
					
					merged = TRUE;
				} else {
					/* if it is going to be too long, make sure we include the
					   separating whitespace */
					word->end = next->start;
					break;
				}
			} else {
				break;
			}
		}
		
		wordl = g_list_next (wordl);
	}
	
	*wordsp = words;
	
	return merged;
}

/* encodes a phrase sequence (different quoting/encoding rules to strings) */
char *
camel_header_encode_phrase (const unsigned char *in)
{
	struct _phrase_word *word = NULL, *last_word = NULL;
	GList *words, *wordl;
	const char *charset;
	GString *out;
	char *outstr;
	
	if (in == NULL)
		return NULL;
	
	words = header_encode_phrase_get_words (in);
	if (!words)
		return NULL;
	
	while (header_encode_phrase_merge_words (&words))
		;
	
	out = g_string_new ("");
	
	/* output words now with spaces between them */
	wordl = words;
	while (wordl) {
		const char *start;
		size_t len;
		
		word = wordl->data;
		
		/* append correct number of spaces between words */
		if (last_word && !(last_word->type == WORD_2047 && word->type == WORD_2047)) {
			/* one or both of the words are not encoded so we write the spaces out untouched */
			len = word->start - last_word->end;
			out = g_string_append_len (out, last_word->end, len);
		}
		
		switch (word->type) {
		case WORD_ATOM:
			out = g_string_append_len (out, word->start, word->end - word->start);
			break;
		case WORD_QSTRING:
			quote_word (out, TRUE, word->start, word->end - word->start);
			break;
		case WORD_2047:
			if (last_word && last_word->type == WORD_2047) {
				/* include the whitespace chars between these 2 words in the
                                   resulting rfc2047 encoded word. */
				len = word->end - last_word->end;
				start = last_word->end;
				
				/* encoded words need to be separated by linear whitespace */
				g_string_append_c (out, ' ');
			} else {
				len = word->end - word->start;
				start = word->start;
			}
			
			if (word->encoding == 1) {
				rfc2047_encode_word (out, start, len, "ISO-8859-1", CAMEL_MIME_IS_PSAFE);
			} else {
				if (!(charset = camel_charset_best (start, len)))
					charset = "UTF-8";
				rfc2047_encode_word (out, start, len, charset, CAMEL_MIME_IS_PSAFE);
			}
			break;
		}
		
		g_free (last_word);
		wordl = g_list_next (wordl);
		
		last_word = word;
	}
	
	/* and we no longer need the list */
	g_free (word);
	g_list_free (words);
	
	outstr = out->str;
	g_string_free (out, FALSE);
	
	return outstr;
}


/* these are all internal parser functions */

/*
   <"> * ( <any char except <"> \, cr  /  \ <any char> ) <">
*/
static char *
header_decode_quoted_string(const char **in)
{
	const char *inptr = *in;
	char *out = NULL, *outptr;
	size_t outlen;
	int c;

	header_decode_lwsp(&inptr);
	if (*inptr == '"') {
		const char *intmp;
		int skip = 0;

		/* first, calc length */
		inptr++;
		intmp = inptr;
		while ( (c = *intmp++) && c!= '"') {
			if (c=='\\' && *intmp) {
				intmp++;
				skip++;
			}
		}
		outlen = intmp-inptr-skip;
		out = outptr = g_malloc(outlen+1);
		while ( (c = *inptr++) && c!= '"') {
			if (c=='\\' && *inptr) {
				c = *inptr++;
			}
			*outptr++ = c;
		}
		*outptr = '\0';
	}
	*in = inptr;
	return out;
}

static char *
header_decode_atom(const char **in)
{
	const char *inptr = *in, *start;

	header_decode_lwsp(&inptr);
	start = inptr;
	while (camel_mime_is_atom(*inptr))
		inptr++;
	*in = inptr;
	if (inptr > start)
		return g_strndup(start, inptr-start);
	else
		return NULL;
}

static char *
header_decode_word (const char **in)
{
	const char *inptr = *in;
	
	header_decode_lwsp (&inptr);
	if (*inptr == '"') {
		*in = inptr;
		return header_decode_quoted_string (in);
	} else {
		*in = inptr;
		return header_decode_atom (in);
	}
}

/* for decoding email addresses, canonically */
static char *
header_decode_domain(const char **in)
{
	const char *inptr = *in, *start;
	int go = TRUE;
	char *ret;
	GString *domain = g_string_new("");

				/* domain ref | domain literal */
	header_decode_lwsp(&inptr);
	while (go) {
		if (*inptr == '[') { /* domain literal */
			domain = g_string_append_c(domain, '[');
			inptr++;
			header_decode_lwsp(&inptr);
			start = inptr;
			while (camel_mime_is_dtext(*inptr)) {
				domain = g_string_append_c(domain, *inptr);
				inptr++;
			}
			if (*inptr == ']') {
				domain = g_string_append_c(domain, ']');
				inptr++;
			} else {
				w(g_warning("closing ']' not found in domain: %s", *in));
			}
		} else {
			char *a = header_decode_atom(&inptr);
			if (a) {
				domain = g_string_append(domain, a);
				g_free(a);
			} else {
				w(g_warning("missing atom from domain-ref"));
				break;
			}
		}
		header_decode_lwsp(&inptr);
		if (*inptr == '.') { /* next sub-domain? */
			domain = g_string_append_c(domain, '.');
			inptr++;
			header_decode_lwsp(&inptr);
		} else
			go = FALSE;
	}

	*in = inptr;

	ret = domain->str;
	g_string_free(domain, FALSE);
	return ret;
}

/*
  address:
   word *('.' word) @ domain |
   *(word) '<' [ *('@' domain ) ':' ] word *( '.' word) @ domain |

   1*word ':' [ word ... etc (mailbox, as above) ] ';'
 */

/* mailbox:
   word *( '.' word ) '@' domain
   *(word) '<' [ *('@' domain ) ':' ] word *( '.' word) @ domain
   */

static struct _camel_header_address *
header_decode_mailbox(const char **in, const char *charset)
{
	const char *inptr = *in;
	char *pre;
	int closeme = FALSE;
	GString *addr;
	GString *name = NULL;
	struct _camel_header_address *address = NULL;
	const char *comment = NULL;

	addr = g_string_new("");

	/* for each address */
	pre = header_decode_word (&inptr);
	header_decode_lwsp(&inptr);
	if (!(*inptr == '.' || *inptr == '@' || *inptr==',' || *inptr=='\0')) {
		/* ',' and '\0' required incase it is a simple address, no @ domain part (buggy writer) */
		name = g_string_new ("");
		while (pre) {
			char *text, *last;

			/* perform internationalised decoding, and append */
			text = camel_header_decode_string (pre, charset);
			g_string_append (name, text);
			last = pre;
			g_free(text);

			pre = header_decode_word (&inptr);
			if (pre) {
				size_t l = strlen (last);
				size_t p = strlen (pre);
				
				/* dont append ' ' between sucsessive encoded words */
				if ((l>6 && last[l-2] == '?' && last[l-1] == '=')
				    && (p>6 && pre[0] == '=' && pre[1] == '?')) {
					/* dont append ' ' */
				} else {
					name = g_string_append_c(name, ' ');
				}
			} else {
				/* Fix for stupidly-broken-mailers that like to put '.''s in names unquoted */
				/* see bug #8147 */
				while (!pre && *inptr && *inptr != '<') {
					w(g_warning("Working around stupid mailer bug #5: unescaped characters in names"));
					name = g_string_append_c(name, *inptr++);
					pre = header_decode_word (&inptr);
				}
			}
			g_free(last);
		}
		header_decode_lwsp(&inptr);
		if (*inptr == '<') {
			closeme = TRUE;
		try_address_again:
			inptr++;
			header_decode_lwsp(&inptr);
			if (*inptr == '@') {
				while (*inptr == '@') {
					inptr++;
					header_decode_domain(&inptr);
					header_decode_lwsp(&inptr);
					if (*inptr == ',') {
						inptr++;
						header_decode_lwsp(&inptr);
					}
				}
				if (*inptr == ':') {
					inptr++;
				} else {
					w(g_warning("broken route-address, missing ':': %s", *in));
				}
			}
			pre = header_decode_word (&inptr);
			header_decode_lwsp(&inptr);
		} else {
			w(g_warning("broken address? %s", *in));
		}
	}

	if (pre) {
		addr = g_string_append(addr, pre);
	} else {
		w(g_warning("No local-part for email address: %s", *in));
	}

	/* should be at word '.' localpart */
	while (*inptr == '.' && pre) {
		inptr++;
		g_free(pre);
		pre = header_decode_word (&inptr);
		addr = g_string_append_c(addr, '.');
		if (pre)
			addr = g_string_append(addr, pre);
		comment = inptr;
		header_decode_lwsp(&inptr);
	}
	g_free(pre);

	/* now at '@' domain part */
	if (*inptr == '@') {
		char *dom;

		inptr++;
		addr = g_string_append_c(addr, '@');
		comment = inptr;
		dom = header_decode_domain(&inptr);
		addr = g_string_append(addr, dom);
		g_free(dom);
	} else if (*inptr != '>' || !closeme) {
		/* If we get a <, the address was probably a name part, lets try again shall we? */
		/* Another fix for seriously-broken-mailers */
		if (*inptr && *inptr != ',') {
			char *text;

			w(g_warning("We didn't get an '@' where we expected in '%s', trying again", *in));
			w(g_warning("Name is '%s', Addr is '%s' we're at '%s'\n", name?name->str:"<UNSET>", addr->str, inptr));

			/* need to keep *inptr, as try_address_again will drop the current character */
			if (*inptr == '<')
				closeme = TRUE;
			else
				g_string_append_c(addr, *inptr);

			/* check for address is encoded word ... */
			text = camel_header_decode_string(addr->str, charset);
			if (name == NULL) {
				name = addr;
				addr = g_string_new("");
				if (text) {
					g_string_truncate(name, 0);
					g_string_append(name, text);
				}
			} else {
				g_string_append(name, text?text:addr->str);
				g_string_truncate(addr, 0);
			}
			g_free(text);

			/* or maybe that we've added up a bunch of broken bits to make an encoded word */
			text = rfc2047_decode_word(name->str, name->len);
			if (text) {
				g_string_truncate(name, 0);
				g_string_append(name, text);
				g_free(text);
			}

			goto try_address_again;
		}
		w(g_warning("invalid address, no '@' domain part at %c: %s", *inptr, *in));
	}

	if (closeme) {
		header_decode_lwsp(&inptr);
		if (*inptr == '>') {
			inptr++;
		} else {
			w(g_warning("invalid route address, no closing '>': %s", *in));
		} 
	} else if (name == NULL && comment != NULL && inptr>comment) { /* check for comment after address */
		char *text, *tmp;
		const char *comstart, *comend;

		/* this is a bit messy, we go from the last known position, because
		   decode_domain/etc skip over any comments on the way */
		/* FIXME: This wont detect comments inside the domain itself,
		   but nobody seems to use that feature anyway ... */

		d(printf("checking for comment from '%s'\n", comment));

		comstart = strchr(comment, '(');
		if (comstart) {
			comstart++;
			header_decode_lwsp(&inptr);
			comend = inptr-1;
			while (comend > comstart && comend[0] != ')')
				comend--;
			
			if (comend > comstart) {
				d(printf("  looking at subset '%.*s'\n", comend-comstart, comstart));
				tmp = g_strndup (comstart, comend-comstart);
				text = camel_header_decode_string (tmp, charset);
				name = g_string_new (text);
				g_free (tmp);
				g_free (text);
			}
		}
	}
	
	*in = inptr;
	
	if (addr->len > 0) {
		if (!g_utf8_validate (addr->str, addr->len, NULL)) {
			/* workaround for invalid addr-specs containing 8bit chars (see bug #42170 for details) */
			const char *locale_charset;
			GString *out;
			
			locale_charset = e_iconv_locale_charset ();
			
			out = g_string_new ("");
			
			if ((charset == NULL || !append_8bit (out, addr->str, addr->len, charset))
			    && (locale_charset == NULL || !append_8bit (out, addr->str, addr->len, locale_charset)))
				append_latin1 (out, addr->str, addr->len);
			
			g_string_free (addr, TRUE);
			addr = out;
		}
		
		address = camel_header_address_new_name(name ? name->str : "", addr->str);
	}
	
	d(printf("got mailbox: %s\n", addr->str));
	
	g_string_free(addr, TRUE);
	if (name)
		g_string_free(name, TRUE);
	
	return address;
}

static struct _camel_header_address *
header_decode_address(const char **in, const char *charset)
{
	const char *inptr = *in;
	char *pre;
	GString *group = g_string_new("");
	struct _camel_header_address *addr = NULL, *member;

	/* pre-scan, trying to work out format, discard results */
	header_decode_lwsp(&inptr);
	while ((pre = header_decode_word (&inptr))) {
		group = g_string_append(group, pre);
		group = g_string_append(group, " ");
		g_free(pre);
	}
	header_decode_lwsp(&inptr);
	if (*inptr == ':') {
		d(printf("group detected: %s\n", group->str));
		addr = camel_header_address_new_group(group->str);
		/* that was a group spec, scan mailbox's */
		inptr++;
		/* FIXME: check rfc 2047 encodings of words, here or above in the loop */
		header_decode_lwsp(&inptr);
		if (*inptr != ';') {
			int go = TRUE;
			do {
				member = header_decode_mailbox(&inptr, charset);
				if (member)
					camel_header_address_add_member(addr, member);
				header_decode_lwsp(&inptr);
				if (*inptr == ',')
					inptr++;
				else
					go = FALSE;
			} while (go);
			if (*inptr == ';') {
				inptr++;
			} else {
				w(g_warning("Invalid group spec, missing closing ';': %s", *in));
			}
		} else {
			inptr++;
		}
		*in = inptr;
	} else {
		addr = header_decode_mailbox(in, charset);
	}

	g_string_free(group, TRUE);

	return addr;
}

struct _camel_header_address *
camel_header_address_decode(const char *in, const char *charset)
{
	const char *inptr = in, *last;
	struct _camel_header_address *list = NULL, *addr;

	d(printf("decoding To: '%s'\n", in));

	if (in == NULL)
		return NULL;

	header_decode_lwsp(&inptr);
	if (*inptr == 0)
		return NULL;

	do {
		last = inptr;
		addr = header_decode_address(&inptr, charset);
		if (addr)
			camel_header_address_list_append(&list, addr);
		header_decode_lwsp(&inptr);
		if (*inptr == ',')
			inptr++;
		else
			break;
	} while (inptr != last);

	if (*inptr) {
		w(g_warning("Invalid input detected at %c (%d): %s\n or at: %s", *inptr, inptr-in, in, inptr));
	}

	if (inptr == last) {
		w(g_warning("detected invalid input loop at : %s", last));
	}

	return list;
}

/* ok, here's the address stuff, what a mess ... */
struct _camel_header_address *
camel_header_address_new (void)
{
	struct _camel_header_address *h;
	h = g_malloc0(sizeof(*h));
	h->type = CAMEL_HEADER_ADDRESS_NONE;
	h->refcount = 1;
	return h;
}

struct _camel_header_address *
camel_header_address_new_name(const char *name, const char *addr)
{
	struct _camel_header_address *h;
	h = camel_header_address_new();
	h->type = CAMEL_HEADER_ADDRESS_NAME;
	h->name = g_strdup(name);
	h->v.addr = g_strdup(addr);
	return h;
}

struct _camel_header_address *
camel_header_address_new_group (const char *name)
{
	struct _camel_header_address *h;

	h = camel_header_address_new();
	h->type = CAMEL_HEADER_ADDRESS_GROUP;
	h->name = g_strdup(name);
	return h;
}

void
camel_header_address_ref(struct _camel_header_address *h)
{
	if (h)
		h->refcount++;
}

void
camel_header_address_unref(struct _camel_header_address *h)
{
	if (h) {
		if (h->refcount <= 1) {
			if (h->type == CAMEL_HEADER_ADDRESS_GROUP) {
				camel_header_address_list_clear(&h->v.members);
			} else if (h->type == CAMEL_HEADER_ADDRESS_NAME) {
				g_free(h->v.addr);
			}
			g_free(h->name);
			g_free(h);
		} else {
			h->refcount--;
		}
	}
}

void
camel_header_address_set_name(struct _camel_header_address *h, const char *name)
{
	if (h) {
		g_free(h->name);
		h->name = g_strdup(name);
	}
}

void
camel_header_address_set_addr(struct _camel_header_address *h, const char *addr)
{
	if (h) {
		if (h->type == CAMEL_HEADER_ADDRESS_NAME
		    || h->type == CAMEL_HEADER_ADDRESS_NONE) {
			h->type = CAMEL_HEADER_ADDRESS_NAME;
			g_free(h->v.addr);
			h->v.addr = g_strdup(addr);
		} else {
			g_warning("Trying to set the address on a group");
		}
	}
}

void
camel_header_address_set_members(struct _camel_header_address *h, struct _camel_header_address *group)
{
	if (h) {
		if (h->type == CAMEL_HEADER_ADDRESS_GROUP
		    || h->type == CAMEL_HEADER_ADDRESS_NONE) {
			h->type = CAMEL_HEADER_ADDRESS_GROUP;
			camel_header_address_list_clear(&h->v.members);
			/* should this ref them? */
			h->v.members = group;
		} else {
			g_warning("Trying to set the members on a name, not group");
		}
	}
}

static void
camel_header_address_add_member(struct _camel_header_address *h, struct _camel_header_address *member)
{
	if (h) {
		if (h->type == CAMEL_HEADER_ADDRESS_GROUP
		    || h->type == CAMEL_HEADER_ADDRESS_NONE) {
			h->type = CAMEL_HEADER_ADDRESS_GROUP;
			camel_header_address_list_append(&h->v.members, member);
		}		    
	}
}

static void
camel_header_address_list_append_list(struct _camel_header_address **l, struct _camel_header_address **h)
{
	if (l) {
		struct _camel_header_address *n = (struct _camel_header_address *)l;

		while (n->next)
			n = n->next;
		n->next = *h;
	}
}


static void
camel_header_address_list_append(struct _camel_header_address **l, struct _camel_header_address *h)
{
	if (h) {
		camel_header_address_list_append_list(l, &h);
		h->next = NULL;
	}
}

void
camel_header_address_list_clear(struct _camel_header_address **l)
{
	struct _camel_header_address *a, *n;
	a = *l;
	while (a) {
		n = a->next;
		camel_header_address_unref(a);
		a = n;
	}
	*l = NULL;
}

/* if encode is true, then the result is suitable for mailing, otherwise
   the result is suitable for display only (and may not even be re-parsable) */
static void
header_address_list_encode_append (GString *out, int encode, struct _camel_header_address *a)
{
	char *text;
	
	while (a) {
		switch (a->type) {
		case CAMEL_HEADER_ADDRESS_NAME:
			if (encode)
				text = camel_header_encode_phrase (a->name);
			else
				text = a->name;
			if (text && *text)
				g_string_append_printf (out, "%s <%s>", text, a->v.addr);
			else
				g_string_append (out, a->v.addr);
			if (encode)
				g_free (text);
			break;
		case CAMEL_HEADER_ADDRESS_GROUP:
			if (encode)
				text = camel_header_encode_phrase (a->name);
			else
				text = a->name;
			g_string_append_printf (out, "%s: ", text);
			header_address_list_encode_append (out, encode, a->v.members);
			g_string_append_printf (out, ";");
			if (encode)
				g_free (text);
			break;
		default:
			g_warning ("Invalid address type");
			break;
		}
		a = a->next;
		if (a)
			g_string_append (out, ", ");
	}
}

char *
camel_header_address_fold (const char *in, size_t headerlen)
{
	size_t len, outlen;
	const char *inptr = in, *space, *p, *n;
	GString *out;
	char *ret;
	int i, needunfold = FALSE;
	
	if (in == NULL)
		return NULL;
	
	/* first, check to see if we even need to fold */
	len = headerlen + 2;
	p = in;
	while (*p) {
		n = strchr (p, '\n');
		if (n == NULL) {
			len += strlen (p);
			break;
		}
		
		needunfold = TRUE;
		len += n-p;
		
		if (len >= CAMEL_FOLD_SIZE)
			break;
		len = 0;
		p = n + 1;
	}
	if (len < CAMEL_FOLD_SIZE)
		return g_strdup (in);
	
	/* we need to fold, so first unfold (if we need to), then process */
	if (needunfold)
		inptr = in = camel_header_unfold (in);
	
	out = g_string_new ("");
	outlen = headerlen + 2;
	while (*inptr) {
		space = strchr (inptr, ' ');
		if (space) {
			len = space - inptr + 1;
		} else {
			len = strlen (inptr);
		}
		
		d(printf("next word '%.*s'\n", len, inptr));
		
		if (outlen + len > CAMEL_FOLD_SIZE) {
			d(printf("outlen = %d wordlen = %d\n", outlen, len));
			/* strip trailing space */
			if (out->len > 0 && out->str[out->len-1] == ' ')
				g_string_truncate (out, out->len-1);
			g_string_append (out, "\n\t");
			outlen = 1;
		}
		
		outlen += len;
		for (i = 0; i < len; i++) {
			g_string_append_c (out, inptr[i]);
		}
		
		inptr += len;
	}
	ret = out->str;
	g_string_free (out, FALSE);
	
	if (needunfold)
		g_free ((char *)in);
	
	return ret;	
}


static char *
camel_header_unfold(const char *in)
{
	char *out = g_malloc(strlen(in)+1);
	const char *inptr = in;
	char c, *o = out;

	o = out;
	while ((c = *inptr++)) {
		if (c == '\n') {
			if (camel_mime_is_lwsp(*inptr)) {
				do {
					inptr++;
				} while (camel_mime_is_lwsp(*inptr));
				*o++ = ' ';
			} else {
				*o++ = c;
			}
		} else {
			*o++ = c;
		}
	}
	*o = 0;

	return out;
}
