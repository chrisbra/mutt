/*
 * Copyright (C) 1996-2000,2009 Michael R. Elkins <me@mutt.org>
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

#define HELP_C

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mutt_curses.h"
#include "keymap.h"
#include "pager.h"
#include "mapping.h"

#include <ctype.h>
#include <string.h>

static const struct menu_func_op_t *help_lookupFunction (int op, int menu)
{
  int i;
  const struct menu_func_op_t *map;

  if (menu != MENU_PAGER && menu != MENU_GENERIC)
  {
    /* first look in the generic map for the function */
    for (i = 0; OpGeneric[i].name; i++)
      if (OpGeneric[i].op == op)
	return (&OpGeneric[i]);
  }

  if ((map = km_get_table(menu)))
  {
    for (i = 0; map[i].name; i++)
      if (map[i].op == op)
	return (&map[i]);
  }

  return (NULL);
}

void mutt_make_help (char *d, size_t dlen, const char *txt, int menu, int op)
{
  char buf[SHORT_STRING];

  if (km_expand_key (buf, sizeof (buf), km_find_func (menu, op)) ||
      km_expand_key (buf, sizeof (buf), km_find_func (MENU_GENERIC, op)))
    snprintf (d, dlen, "%s:%s", buf, txt);
  else
    d[0] = 0;
}

char *
mutt_compile_help (char *buf, size_t buflen, int menu, const struct mapping_t *items)
{
  int i;
  size_t len;
  char *pbuf = buf;

  for (i = 0; items[i].name && buflen > 2; i++)
  {
    if (i)
    {
      *pbuf++ = ' ';
      *pbuf++ = ' ';
      buflen -= 2;
    }
    mutt_make_help (pbuf, buflen, _(items[i].name), menu, items[i].value);
    len = mutt_strlen (pbuf);
    pbuf += len;
    buflen -= len;
  }
  return buf;
}

static int print_macro (FILE *f, int maxwidth, const char **macro)
{
  int n = maxwidth;
  wchar_t wc;
  int w;
  size_t k;
  size_t len = mutt_strlen (*macro);
  mbstate_t mbstate1, mbstate2;

  memset (&mbstate1, 0, sizeof (mbstate1));
  memset (&mbstate2, 0, sizeof (mbstate2));
  for (; len && (k = mbrtowc (&wc, *macro, len, &mbstate1)); *macro += k, len -= k)
  {
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      if (k == (size_t)(-1))
        memset (&mbstate1, 0, sizeof (mbstate1));
      k = (k == (size_t)(-1)) ? 1 : len;
      wc = replacement_char ();
    }
    /* glibc-2.1.3's wcwidth() returns 1 for unprintable chars! */
    if (IsWPrint (wc) && (w = wcwidth (wc)) >= 0)
    {
      if (w > n)
	break;
      n -= w;
      {
	char buf[MB_LEN_MAX*2];
	size_t n1, n2;
	if ((n1 = wcrtomb (buf, wc, &mbstate2)) != (size_t)(-1) &&
	    (n2 = wcrtomb (buf + n1, 0, &mbstate2)) != (size_t)(-1))
	  fputs (buf, f);
      }
    }
    else if (wc < 0x20 || wc == 0x7f)
    {
      if (2 > n)
	break;
      n -= 2;
      if (wc == '\033')
	fprintf (f, "\\e");
      else if (wc == '\n')
	fprintf (f, "\\n");
      else if (wc == '\r')
	fprintf (f, "\\r");
      else if (wc == '\t')
	fprintf (f, "\\t");
      else
	fprintf (f, "^%c", (char)((wc + '@') & 0x7f));
    }
    else
    {
      if (1 > n)
	break;
      n -= 1;
      fprintf (f, "?");
    }
  }
  return (maxwidth - n);
}

static int get_wrapped_width (const char *t, size_t wid)
{
  wchar_t wc;
  size_t k;
  size_t m, n;
  size_t len = mutt_strlen (t);
  const char *s = t;
  mbstate_t mbstate;

  memset (&mbstate, 0, sizeof (mbstate));
  for (m = wid, n = 0;
       len && (k = mbrtowc (&wc, s, len, &mbstate)) && (n <= wid);
       s += k, len -= k)
  {
    if (*s == ' ')
      m = n;
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      if (k == (size_t)(-1))
        memset (&mbstate, 0, sizeof (mbstate));
      k = (k == (size_t)(-1)) ? 1 : len;
      wc = replacement_char ();
    }
    if (!IsWPrint (wc))
      wc = '?';
    n += wcwidth (wc);
  }
  if (n > wid)
    n = m;
  else
    n = wid;
  return n;
}

static int pad (FILE *f, int col, int i)
{
  char fmt[15];  /* min size to accommodate %%-%ds */

  if (col < i)
  {
    snprintf (fmt, sizeof(fmt), "%%-%ds", i - col);
    fprintf (f, fmt, "");
    return (i);
  }
  fputc (' ', f);
  return (col + 1);
}

static void format_line (FILE *f, int ismacro,
			 const char *t1, const char *t2, const char *t3)
{
  int col;
  int col_a, col_b;
  int split;
  int n;

  fputs (t1, f);

  /* don't try to press string into one line with less than 40 characters. */
  split = (MuttIndexWindow->cols < 40);
  if (split)
  {
    col_a = col = 0;
    col_b = LONG_STRING;
    fputc ('\n', f);
  }
  else
  {
    col_a = MuttIndexWindow->cols > 83 ? (MuttIndexWindow->cols - 32) >> 2 : 12;
    col_b = MuttIndexWindow->cols > 49 ? (MuttIndexWindow->cols - 10) >> 1 : 19;
    col = pad (f, mutt_strwidth(t1), col_a);
  }

  if (ismacro > 0)
  {
    if (!Pager || !mutt_strcmp (Pager, "builtin"))
      fputs ("_\010", f);
    fputs ("M ", f);
    col += 2;

    if (!split)
    {
      col += print_macro (f, col_b - col - 4, &t2);
      if (mutt_strwidth (t2) > col_b - col)
	t2 = "...";
    }
  }

  col += print_macro (f, col_b - col - 1, &t2);
  if (split)
    fputc ('\n', f);
  else
    col = pad (f, col, col_b);

  if (split)
  {
    print_macro (f, LONG_STRING, &t3);
    fputc ('\n', f);
  }
  else
  {
    while (*t3)
    {
      n = MuttIndexWindow->cols - col;

      if (ismacro >= 0)
      {
	SKIPWS(t3);
	n = get_wrapped_width (t3, n);
      }

      n = print_macro (f, n, &t3);

      if (*t3)
      {
        if (Pager && mutt_strcmp (Pager, "builtin"))
	{
	  fputc ('\n', f);
	  n = 0;
	}
	else
	{
	  n += col - MuttIndexWindow->cols;
	  if (option (OPTMARKERS))
	    ++n;
	}
	col = pad (f, n, col_b);
      }
    }
  }

  fputc ('\n', f);
}

static void dump_menu (FILE *f, int menu)
{
  struct keymap_t *map;
  const struct menu_func_op_t *b;
  char buf[SHORT_STRING];

  /* browse through the keymap table */
  for (map = Keymaps[menu]; map; map = map->next)
  {
    if (map->op != OP_NULL)
    {
      km_expand_key (buf, sizeof (buf), map);

      if (map->op == OP_MACRO)
      {
	if (map->descr == NULL)
	  format_line (f, -1, buf, "macro", map->macro);
        else
	  format_line (f, 1, buf, map->macro, map->descr);
      }
      else
      {
	b = help_lookupFunction (map->op, menu);
	format_line (f, 0, buf, b ? b->name : "UNKNOWN",
                     b ? _(HelpStrings[b->op]) : _("ERROR: please report this bug"));
      }
    }
  }
}

static int is_bound (struct keymap_t *map, int op)
{
  for (; map; map = map->next)
    if (map->op == op)
      return 1;
  return 0;
}

static void dump_unbound (FILE *f,
			  const struct menu_func_op_t *funcs,
			  struct keymap_t *map,
			  struct keymap_t *aux)
{
  int i;

  for (i = 0; funcs[i].name; i++)
  {
    if (! is_bound (map, funcs[i].op) &&
	(!aux || ! is_bound (aux, funcs[i].op)))
      format_line (f, 0, funcs[i].name, "", _(HelpStrings[funcs[i].op]));
  }
}

void mutt_help (int menu)
{
  BUFFER *t = NULL;
  char buf[SHORT_STRING];
  const char *desc;
  FILE *f;
  const struct menu_func_op_t *funcs;

  /* We don't use the buffer pool because of the extended lifetime of t */
  t = mutt_buffer_new ();
  mutt_buffer_mktemp (t);

  funcs = km_get_table (menu);
  desc = mutt_getnamebyvalue (menu, Menus);
  if (!desc)
    desc = _("<UNKNOWN>");

  do
  {
    if ((f = safe_fopen (mutt_b2s (t), "w")) == NULL)
    {
      mutt_perror (mutt_b2s (t));
      goto cleanup;
    }

    dump_menu (f, menu);
    if (menu != MENU_EDITOR && menu != MENU_PAGER && menu != MENU_GENERIC)
    {
      fputs (_("\nGeneric bindings:\n\n"), f);
      dump_menu (f, MENU_GENERIC);
    }

    fputs (_("\nUnbound functions:\n\n"), f);
    if (funcs)
      dump_unbound (f, funcs, Keymaps[menu], NULL);
    if (menu != MENU_EDITOR && menu != MENU_PAGER && menu != MENU_GENERIC)
      dump_unbound (f, OpGeneric, Keymaps[MENU_GENERIC], Keymaps[menu]);

    safe_fclose (&f);

    snprintf (buf, sizeof (buf), _("Help for %s"), desc);
  }
  while
    (mutt_do_pager (buf, mutt_b2s (t),
		    MUTT_PAGER_RETWINCH | MUTT_PAGER_MARKER | MUTT_PAGER_NSKIP | MUTT_PAGER_NOWRAP,
		    NULL)
     == OP_REFORMAT_WINCH);

cleanup:
  mutt_buffer_free (&t);
}
