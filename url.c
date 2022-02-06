/*
 * Copyright (C) 2000-2002,2004 Thomas Roessler <roessler@does-not-exist.org>
 *
 *     This program is free software; you can redistribute it and/or modify
 *     it under the terms of the GNU General Public License as published by
 *     the Free Software Foundation; either version 2 of the License, or
 *     (at your option) any later version.
 *
 *     This program is distributed in the hope that it will be useful,
 *     but WITHOUT ANY WARRANTY; without even the implied warranty of
 *     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *     GNU General Public License for more details.
 *
 *     You should have received a copy of the GNU General Public License
 *     along with this program; if not, write to the Free Software
 *     Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */

/*
 * A simple URL parser.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mapping.h"
#include "url.h"

#include "mime.h"
#include "rfc2047.h"

#include <ctype.h>

static const struct mapping_t UrlMap[] =
{
  { "file", 	U_FILE },
  { "imap", 	U_IMAP },
  { "imaps", 	U_IMAPS },
  { "pop",  	U_POP },
  { "pops", 	U_POPS },
  { "mailto",	U_MAILTO },
  { "smtp",     U_SMTP },
  { "smtps",    U_SMTPS },
  { NULL,	U_UNKNOWN }
};

static int url_pct_decode (char *s)
{
  char *d;

  if (!s)
    return -1;

  for (d = s; *s; s++)
  {
    if (*s == '%')
    {
      if (s[1] && s[2] &&
	  isxdigit ((unsigned char) s[1]) &&
	  isxdigit ((unsigned char) s[2]) &&
	  hexval (s[1]) >= 0 && hexval (s[2]) >= 0)
      {
	*d++ = (hexval (s[1]) << 4) | (hexval (s[2]));
	s += 2;
      }
      else
	return -1;
    }
    else
      *d++ = *s;
  }
  *d ='\0';
  return 0;
}

url_scheme_t url_check_scheme (const char *s)
{
  char sbuf[STRING];
  char *t;
  int i;

  if (!s || !(t = strchr (s, ':')))
    return U_UNKNOWN;
  if ((size_t)(t - s) >= sizeof (sbuf) - 1)
    return U_UNKNOWN;

  strfcpy (sbuf, s, t - s + 1);
  for (t = sbuf; *t; t++)
    *t = ascii_tolower (*t);

  if ((i = mutt_getvaluebyname (sbuf, UrlMap)) == -1)
    return U_UNKNOWN;
  else
    return (url_scheme_t) i;
}

int url_parse_file (char *d, const char *src, size_t dl)
{
  if (ascii_strncasecmp (src, "file:", 5))
    return -1;
  else if (!ascii_strncasecmp (src, "file://", 7))	/* we don't support remote files */
    return -1;
  else
    strfcpy (d, src + 5, dl);

  return url_pct_decode (d);
}

/* ciss_parse_userhost: fill in components of ciss with info from src. Note
 *   these are pointers into src, which is altered with '\0's. Port of 0
 *   means no port given. */
static int ciss_parse_userhost (ciss_url_t *ciss, char *src)
{
  char *t, *p;

  ciss->user = NULL;
  ciss->pass = NULL;
  ciss->host = NULL;
  ciss->port = 0;

  if (strncmp (src, "//", 2) != 0)
  {
    ciss->path = src;
    return url_pct_decode (ciss->path);
  }

  src += 2;

  if ((ciss->path = strchr (src, '/')))
    *ciss->path++ = '\0';

  if ((t = strrchr (src, '@')))
  {
    *t = '\0';
    if ((p = strchr (src, ':')))
    {
      *p = '\0';
      ciss->pass = p + 1;
      if (url_pct_decode (ciss->pass) < 0)
	return -1;
    }
    ciss->user = src;
    if (url_pct_decode (ciss->user) < 0)
      return -1;
    src = t + 1;
  }

  /* IPv6 literal address.  It may contain colons, so set t to start
   * the port scan after it.
   */
  if ((*src == '[') && (t = strchr (src, ']')))
  {
    src++;
    *t++ = '\0';
  }
  else
    t = src;

  if ((p = strchr (t, ':')))
  {
    int t;
    *p++ = '\0';
    if (mutt_atoi (p, &t, MUTT_ATOI_ALLOW_EMPTY) < 0 || t < 0 || t > 0xffff)
      return -1;
    ciss->port = (unsigned short)t;
  }
  else
    ciss->port = 0;

  ciss->host = src;
  return url_pct_decode (ciss->host) >= 0 &&
    (!ciss->path || url_pct_decode (ciss->path) >= 0) ? 0 : -1;
}

/* url_parse_ciss: Fill in ciss_url_t. char* elements are pointers into src,
 *   which is modified by this call (duplicate it first if you need to). */
int url_parse_ciss (ciss_url_t *ciss, char *src)
{
  char *tmp;

  if ((ciss->scheme = url_check_scheme (src)) == U_UNKNOWN)
    return -1;

  tmp = strchr (src, ':') + 1;

  return ciss_parse_userhost (ciss, tmp);
}

static void url_pct_encode (char *dst, size_t l, const char *src)
{
  static const char *alph = "0123456789ABCDEF";

  *dst = 0;
  l--;
  while (src && *src && l)
  {
    if (strchr ("/:%", *src))
    {
      if (l < 3)
        break;

      *dst++ = '%';
      *dst++ = alph[(*src >> 4) & 0xf];
      *dst++ = alph[*src & 0xf];
      src++;
      l -= 3;
      continue;
    }
    *dst++ = *src++;
    l--;
  }
  *dst = 0;
}

int url_ciss_tostring (ciss_url_t* ciss, char* dest, size_t len, int flags)
{
  BUFFER *dest_buf;
  int retval;

  dest_buf = mutt_buffer_pool_get ();

  retval = url_ciss_tobuffer (ciss, dest_buf, flags);
  if (!retval)
    strfcpy (dest, mutt_b2s (dest_buf), len);

  mutt_buffer_pool_release (&dest_buf);

  return retval;
}

/* url_ciss_tobuffer: output the URL string for a given CISS object. */
int url_ciss_tobuffer (ciss_url_t* ciss, BUFFER* dest, int flags)
{
  if (ciss->scheme == U_UNKNOWN)
    return -1;

  mutt_buffer_printf (dest, "%s:", mutt_getnamebyvalue (ciss->scheme, UrlMap));

  if (ciss->host)
  {
    if (!(flags & U_PATH))
      mutt_buffer_addstr (dest, "//");

    if (ciss->user)
    {
      char u[STRING];
      url_pct_encode (u, sizeof (u), ciss->user);

      if (flags & U_DECODE_PASSWD && ciss->pass)
      {
	char p[STRING];
	url_pct_encode (p, sizeof (p), ciss->pass);
	mutt_buffer_add_printf (dest, "%s:%s@", u, p);
      }
      else
	mutt_buffer_add_printf (dest, "%s@", u);
    }

    if (strchr (ciss->host, ':'))
      mutt_buffer_add_printf (dest, "[%s]", ciss->host);
    else
      mutt_buffer_add_printf (dest, "%s", ciss->host);

    if (ciss->port)
      mutt_buffer_add_printf (dest, ":%hu/", ciss->port);
    else
      mutt_buffer_addstr (dest, "/");
  }

  if (ciss->path)
    mutt_buffer_addstr (dest, ciss->path);

  return 0;
}

/* This is similar to mutt_matches_ignore(), except that it
 * doesn't allow prefix matches.
 */
static int url_mailto_header_allowed (const char *header)
{
  LIST *t = MailtoAllow;

  for (; t; t = t->next)
  {
    if (!ascii_strcasecmp (header, t->data) || *t->data == '*')
      return 1;
  }
  return 0;
}

int url_parse_mailto (ENVELOPE *e, char **body, const char *src)
{
  char *t, *p;
  char *tmp;
  char *headers;
  char *tag, *value;

  int rc = -1;

  LIST *last = NULL;

  if (!(t = strchr (src, ':')))
    return -1;

  /* copy string for safe use of strtok() */
  if ((tmp = safe_strdup (t + 1)) == NULL)
    return -1;

  if ((headers = strchr (tmp, '?')))
    *headers++ = '\0';

  if (url_pct_decode (tmp) < 0)
    goto out;

  e->to = rfc822_parse_adrlist (e->to, tmp);

  tag = headers ? strtok_r (headers, "&", &p) : NULL;

  for (; tag; tag = strtok_r (NULL, "&", &p))
  {
    if ((value = strchr (tag, '=')))
      *value++ = '\0';
    if (!value || !*value)
      continue;

    if (url_pct_decode (tag) < 0)
      goto out;
    if (url_pct_decode (value) < 0)
      goto out;

    mutt_filter_commandline_header_tag (tag);

    /* Determine if this header field is on the allowed list.  Since Mutt
     * interprets some header fields specially (such as
     * "Attach: ~/.gnupg/secring.gpg"), care must be taken to ensure that
     * only safe fields are allowed.
     *
     * RFC2368, "4. Unsafe headers"
     * The user agent interpreting a mailto URL SHOULD choose not to create
     * a message if any of the headers are considered dangerous; it may also
     * choose to create a message with only a subset of the headers given in
     * the URL.
     */
    if (url_mailto_header_allowed (tag))
    {
      if (!ascii_strcasecmp (tag, "body"))
      {
	if (body)
	  mutt_str_replace (body, value);
      }
      /* This is a hack to allow un-bracketed message-ids in mailto URLs
       * without doing the same for email header parsing. */
      else if (!ascii_strcasecmp (tag, "in-reply-to"))
      {
        mutt_free_list (&e->in_reply_to);
        mutt_filter_commandline_header_value (value);
        e->in_reply_to = mutt_parse_references (value, 1);
      }
      else
      {
	char *scratch;
	size_t taglen = mutt_strlen (tag);

        mutt_filter_commandline_header_value (value);
	safe_asprintf (&scratch, "%s: %s", tag, value);
	scratch[taglen] = 0; /* overwrite the colon as mutt_parse_rfc822_line expects */
	value = skip_email_wsp(&scratch[taglen + 1]);
	mutt_parse_rfc822_line (e, NULL, scratch, value, 1, 0, 1, &last);
	FREE (&scratch);
      }
    }
  }

  /* RFC2047 decode after the RFC822 parsing */
  rfc2047_decode_envelope (e);

  rc = 0;

out:
  FREE (&tmp);
  return rc;
}
