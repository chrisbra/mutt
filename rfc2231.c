/*
 * Copyright (C) 1999-2000 Thomas Roessler <roessler@guug.de>
 *
 *     This program is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public
 *     License as published by the Free Software Foundation; either
 *     version 2 of the License, or (at your option) any later
 *     version.
 * 
 *     This program is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied
 *     warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *     PURPOSE.  See the GNU General Public License for more
 *     details.
 * 
 *     You should have received a copy of the GNU General Public
 *     License along with this program; if not, write to the Free
 *     Software Foundation, Inc., 59 Temple Place - Suite 330,
 *     Boston, MA  02111, USA.
 */

/*
 * Yet another MIME encoding for header data.  This time, it's
 * parameters, specified in RFC 2231, and modeled after the
 * encoding used in URLs.
 * 
 * Additionally, continuations and encoding are mixed in an, errrm,
 * interesting manner.
 *
 */

#include "mutt.h"
#include "mime.h"
#include "charset.h"
#include "rfc2047.h"
#include "rfc2231.h"

#include <ctype.h>
#include <string.h>
#include <stdlib.h>

struct rfc2231_parameter
{
  char *attribute;
  char *value;
  int  index;
  int  encoded;
  struct rfc2231_parameter 
       *next;
};

static char *rfc2231_get_charset (char *, char *, size_t);
static struct rfc2231_parameter *rfc2231_new_parameter (void);
static void rfc2231_decode_one (char *, char *);
static void rfc2231_free_parameter (struct rfc2231_parameter **);
static void rfc2231_join_continuations (PARAMETER **, struct rfc2231_parameter *);
static void rfc2231_list_insert (struct rfc2231_parameter **, struct rfc2231_parameter *);

static void purge_empty_parameters (PARAMETER **headp)
{
  PARAMETER *p, *q, **last;
  
  for (last = headp, p = *headp; p; p = q)
  {
    q = p->next;
    if (!p->attribute || !p->value)
    {
      *last = q;
      p->next = NULL;
      mutt_free_parameter (&p);
    }
    else
      last = &p->next;
  }
}


void rfc2231_decode_parameters (PARAMETER **headp)
{
  PARAMETER *head = NULL;
  PARAMETER **last;
  PARAMETER *p, *q;

  struct rfc2231_parameter *conthead = NULL;
  struct rfc2231_parameter *conttmp;

  char *s, *t;
  char charset[STRING];

  int encoded;
  int index;
  short dirty = 0;	/* set to 1 when we may have created
			 * empty parameters.
			 */
  
  if (!headp) return;

  purge_empty_parameters (headp);
  
  for (last = &head, p = *headp; p; p = q)
  {
    q = p->next;

    if (!(s = strchr (p->attribute, '*')))
    {

      /* 
       * Using RFC 2047 encoding in MIME parameters is explicitly
       * forbidden by that document.  Nevertheless, it's being
       * generated by some software, including certain Lotus Notes to 
       * Internet Gateways.  So we actually decode it.
       */

      if (option (OPTRFC2047PARAMS) && p->value && strstr (p->value, "=?"))
	rfc2047_decode (&p->value);

      *last = p;
      last = &p->next;
      p->next = NULL;
    }
    else if (*(s + 1) == '\0')
    {
      *s = '\0';
      
      s = rfc2231_get_charset (p->value, charset, sizeof (charset));
      rfc2231_decode_one (p->value, s);
      mutt_convert_string (&p->value, charset, Charset);

      *last = p;
      last = &p->next;
      p->next = NULL;
      
      dirty = 1;
    }
    else
    {
      *s = '\0'; s++; /* let s point to the first character of index. */
      for (t = s; *t && isdigit (*t); t++)
	;
      encoded = (*t == '*');
      *t = '\0';

      index = atoi (s);

      conttmp = rfc2231_new_parameter ();
      conttmp->attribute = p->attribute;
      conttmp->value = p->value;
      conttmp->encoded = encoded;
      conttmp->index = index;
      
      p->attribute = NULL;
      p->value = NULL;
      safe_free ((void **) &p);

      rfc2231_list_insert (&conthead, conttmp);
    }
  }

  if (conthead)
  {
    rfc2231_join_continuations (last, conthead);
    dirty = 1;
  }
  
  *headp = head;
  
  if (dirty)
    purge_empty_parameters (headp);
}
  
static struct rfc2231_parameter *rfc2231_new_parameter (void)
{
  return safe_calloc (sizeof (struct rfc2231_parameter), 1);
}

static void rfc2231_free_parameter (struct rfc2231_parameter **p)
{
  if (*p)
  {
    safe_free ((void **) &(*p)->attribute);
    safe_free ((void **) &(*p)->value);
    safe_free ((void **) p);
  }
}

static char *rfc2231_get_charset (char *value, char *charset, size_t chslen)
{
  char *t, *u;
  
  if (!(t = strchr (value, '\'')))
  {
    charset[0] = '\0';
    return value;
  }
  
  *t = '\0';
  strfcpy (charset, value, chslen);
  
  if ((u = strchr (t + 1, '\'')))
    return u + 1;
  else
    return t + 1;
}

static void rfc2231_decode_one (char *dest, char *src)
{
  char *d;

  for (d = dest; *src; src++)
  {
    if (*src == '%' && isxdigit (*(src + 1)) && isxdigit (*(src + 2)))
    {
      *d++ = (hexval (*(src + 1)) << 4) | (hexval (*(src + 2)));
      src += 2;
    }
    else
      *d++ = *src;
  }
  
  *d = '\0';
}

/* insert parameter into an ordered list.
 * 
 * Primary sorting key: attribute
 * Secondary sorting key: index
 */

static void rfc2231_list_insert (struct rfc2231_parameter **list,
				 struct rfc2231_parameter *par)
{
  struct rfc2231_parameter **last = list;
  struct rfc2231_parameter *p = *list, *q;
  int c;
  
  while (p)
  {
    last = &p->next;
    q = p; p = p->next;

    c = strcmp (par->value, q->value);
    if ((c > 0) || (c == 0 && par->index >= q->index))
      break;
  }
  
  par->next = p;
  *last = par;
}

/* process continuation parameters */

static void rfc2231_join_continuations (PARAMETER **head,
					struct rfc2231_parameter *par)
{
  struct rfc2231_parameter *q;

  char attribute[STRING];
  char charset[STRING];
  char *value = NULL;
  char *valp;
  int encoded;

  size_t l, vl;
  
  while (par)
  {
    value = NULL; l = 0;
    
    strfcpy (attribute, par->attribute, sizeof (attribute));

    if ((encoded = par->encoded))
      valp = rfc2231_get_charset (par->value, charset, sizeof (charset));
    else
      valp = par->value;

    do 
    {
      if (encoded && par->encoded)
	rfc2231_decode_one (par->value, valp);
      
      vl = strlen (par->value);
      
      safe_realloc ((void **) &value, l + vl + 1);
      strcpy (value + l, par->value);
      l += vl;

      q = par->next;
      rfc2231_free_parameter (&par);
      if ((par = q))
	valp = par->value;
    } while (par && !strcmp (par->attribute, attribute));
    
    if (value)
    {
      if (encoded)
	mutt_convert_string (&value, charset, Charset);
      *head = mutt_new_parameter ();
      (*head)->attribute = safe_strdup (attribute);
      (*head)->value = value;
      head = &(*head)->next;
    }
  }
}

int rfc2231_encode (char *dest, size_t l, unsigned char *src)
{
  char *buff;
  unsigned char *s;
  char *t;
  int encode = 0;

  buff = safe_malloc (3 * strlen ((char *) src) + 1);

  for (s = src; *s && !encode; s++)
  {
    if (*s & 0x80)
      encode = 1;
  }

  if (!encode)
    strfcpy (dest, (char *) src, l);
  else
  {
    for (s = src, t = buff; *s && (t - buff) < sizeof (buff) - 4; s++)
    {
      if ((*s & 0x80) || *s == '\'')
      {
	sprintf ((char *) t, "%%%02x", (unsigned int) *s);
	t += 3;
      }
      else
	*t++ = *s;
    }
    *t = '\0';
    
    if (Charset && SendCharset && mutt_strcasecmp (Charset, SendCharset))
      mutt_convert_string (&buff, Charset, SendCharset);

    snprintf (dest, l, "%s''%s", SendCharset ? SendCharset :
	      (Charset ? Charset : "unknown-8bit"), buff);
  }

  safe_free ((void **) &buff);
  return encode;
}

