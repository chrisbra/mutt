/*
 * Copyright (C) 1996-2002,2012 Michael R. Elkins <me@mutt.org>
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

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mapping.h"
#include "color.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

/* globals */
COLOR_ATTR *ColorQuote;
int ColorQuoteUsed;
COLOR_ATTR ColorDefs[MT_COLOR_MAX];
COLOR_LINE *ColorHdrList = NULL;
COLOR_LINE *ColorBodyList = NULL;
COLOR_LINE *ColorIndexList = NULL;

/* local to this file */
static int ColorQuoteSize;
#if defined(HAVE_COLOR) && defined(HAVE_USE_DEFAULT_COLORS)
static int HaveDefaultColors = 0;
static int DefaultColorsInit = 0;
#endif

#define COLOR_UNSET (-2)

#ifdef HAVE_COLOR

#define COLOR_DEFAULT (-1)

typedef struct color_list
{
  short fg;
  short bg;
  short pair;
  short count;
  unsigned int ansi : 1;
  unsigned int overlay : 1;
  struct color_list *next;
} COLOR_LIST;

/* color type for mutt_alloc_color() */
#define MUTT_COLOR_TYPE_NORMAL     1
#define MUTT_COLOR_TYPE_ANSI       2
#define MUTT_COLOR_TYPE_OVERLAY    3

static COLOR_LIST *ColorList = NULL;
static int UserColors = 0;
static int AnsiColors = 0;

static const struct mapping_t Colors[] =
{
  { "black",	COLOR_BLACK },
  { "blue",	COLOR_BLUE },
  { "cyan",	COLOR_CYAN },
  { "green",	COLOR_GREEN },
  { "magenta",	COLOR_MAGENTA },
  { "red",	COLOR_RED },
  { "white",	COLOR_WHITE },
  { "yellow",	COLOR_YELLOW },
#if defined (USE_SLANG_CURSES) || defined (HAVE_USE_DEFAULT_COLORS)
  { "default",	COLOR_DEFAULT },
#endif
  { 0, 0 }
};

#endif /* HAVE_COLOR */

static const struct mapping_t Fields[] =
{
  { "hdrdefault",	MT_COLOR_HDEFAULT },
  { "quoted",		MT_COLOR_QUOTED },
  { "signature",	MT_COLOR_SIGNATURE },
  { "indicator",	MT_COLOR_INDICATOR },
  { "status",		MT_COLOR_STATUS },
  { "tree",		MT_COLOR_TREE },
  { "error",		MT_COLOR_ERROR },
  { "normal",		MT_COLOR_NORMAL },
  { "tilde",		MT_COLOR_TILDE },
  { "markers",		MT_COLOR_MARKERS },
  { "header",		MT_COLOR_HEADER },
  { "body",		MT_COLOR_BODY },
  { "message",		MT_COLOR_MESSAGE },
  { "attachment",	MT_COLOR_ATTACHMENT },
  { "search",		MT_COLOR_SEARCH },
  { "bold",		MT_COLOR_BOLD },
  { "underline",	MT_COLOR_UNDERLINE },
  { "index",		MT_COLOR_INDEX },
  { "prompt",		MT_COLOR_PROMPT },
#ifdef USE_SIDEBAR
  { "sidebar_divider",	MT_COLOR_DIVIDER },
  { "sidebar_flagged",	MT_COLOR_FLAGGED },
  { "sidebar_highlight",MT_COLOR_HIGHLIGHT },
  { "sidebar_indicator",MT_COLOR_SB_INDICATOR },
  { "sidebar_new",	MT_COLOR_NEW },
  { "sidebar_spoolfile",MT_COLOR_SB_SPOOLFILE },
#endif
  { NULL,		0 }
};

static const struct mapping_t ComposeFields[] =
{
  { "header",               MT_COLOR_COMPOSE_HEADER },
  { "security_encrypt",     MT_COLOR_COMPOSE_SECURITY_ENCRYPT },
  { "security_sign",        MT_COLOR_COMPOSE_SECURITY_SIGN },
  { "security_both",        MT_COLOR_COMPOSE_SECURITY_BOTH },
  { "security_none",        MT_COLOR_COMPOSE_SECURITY_NONE },
  { NULL,                   0 }
};

#define COLOR_QUOTE_INIT	8

#ifdef NCURSES_VERSION
#define ATTR_MASK (A_ATTRIBUTES ^ A_COLOR)
#elif defined (USE_SLANG_CURSES)
#define ATTR_MASK (~(unsigned int)A_NORMAL ^ (A_CHARTEXT | A_UNUSED | A_COLOR))
#endif

#ifdef HAVE_COLOR
static void mutt_free_color (int pair);
#endif


static COLOR_LINE *mutt_new_color_line (void)
{
  COLOR_LINE *p = safe_calloc (1, sizeof (COLOR_LINE));

  p->fg = p->bg = COLOR_UNSET;

  return (p);
}

static void mutt_free_color_line(COLOR_LINE **l,
				 int free_colors)
{
  COLOR_LINE *tmp;

  if (!l || !*l)
    return;

  tmp = *l;

#ifdef HAVE_COLOR
  if (free_colors && tmp->color.pair)
    mutt_free_color (tmp->color.pair);
#endif

  /* we should really introduce a container
   * type for regular expressions.
   */

  regfree(&tmp->rx);
  mutt_pattern_free(&tmp->color_pattern);
  FREE (&tmp->pattern);
  FREE (l);		/* __FREE_CHECKED__ */
}

#if defined(HAVE_COLOR) && defined(HAVE_USE_DEFAULT_COLORS)
static void init_default_colors (void)
{
  DefaultColorsInit = 1;
  if (use_default_colors () == OK)
    HaveDefaultColors = 1;
}
#endif /* defined(HAVE_COLOR) && defined(HAVE_USE_DEFAULT_COLORS) */

void ci_start_color (void)
{
  memset (ColorDefs, 0, sizeof (COLOR_ATTR) * MT_COLOR_MAX);
  ColorQuote = (COLOR_ATTR *) safe_malloc (COLOR_QUOTE_INIT * sizeof (COLOR_ATTR));
  memset (ColorQuote, 0, sizeof (COLOR_ATTR) * COLOR_QUOTE_INIT);
  ColorQuoteSize = COLOR_QUOTE_INIT;
  ColorQuoteUsed = 0;

  /* set some defaults */
  ColorDefs[MT_COLOR_STATUS].attrs = A_REVERSE;
  ColorDefs[MT_COLOR_INDICATOR].attrs = A_REVERSE;
  ColorDefs[MT_COLOR_SEARCH].attrs = A_REVERSE;
  ColorDefs[MT_COLOR_MARKERS].attrs = A_REVERSE;
#ifdef USE_SIDEBAR
  ColorDefs[MT_COLOR_HIGHLIGHT].attrs = A_UNDERLINE;
#endif
  /* special meaning: toggle the relevant attribute */
  ColorDefs[MT_COLOR_BOLD].attrs = 0;
  ColorDefs[MT_COLOR_UNDERLINE].attrs = 0;

#ifdef HAVE_COLOR
  start_color ();
#endif
}

#ifdef HAVE_COLOR

#ifdef USE_SLANG_CURSES
static char *get_color_name (char *dest, size_t destlen, int val)
{
  static const char * const missing[3] = {"brown", "lightgray", "default"};
  int i;

  switch (val)
  {
    case COLOR_YELLOW:
      strfcpy (dest, missing[0], destlen);
      return dest;

    case COLOR_WHITE:
      strfcpy (dest, missing[1], destlen);
      return dest;

    case COLOR_DEFAULT:
      strfcpy (dest, missing[2], destlen);
      return dest;
  }

  for (i = 0; Colors[i].name; i++)
  {
    if (Colors[i].value == val)
    {
      strfcpy (dest, Colors[i].name, destlen);
      return dest;
    }
  }

  /* Sigh. If we got this far, the color is of the form 'colorN'
   * Slang can handle this itself, so just return 'colorN'
   */

  snprintf (dest, destlen, "color%d", val);
  return dest;
}
#endif

static COLOR_LIST *find_color_list_entry_by_pair (int pair)
{
  COLOR_LIST *p = ColorList;

  while (p)
  {
    if (p->pair == pair)
      return p;
    if (p->pair > pair)
      return NULL;
    p = p->next;
  }

  return NULL;
}

#endif /* HAVE_COLOR */

COLOR_ATTR mutt_merge_colors (COLOR_ATTR source, COLOR_ATTR overlay)
{
#ifdef HAVE_COLOR
  COLOR_LIST *source_color_entry, *overlay_color_entry;
  int merged_fg, merged_bg;
#endif
  COLOR_ATTR merged = {0};

#ifdef HAVE_COLOR
  merged.pair = overlay.pair;

  overlay_color_entry = find_color_list_entry_by_pair (overlay.pair);

  if (overlay_color_entry &&
      (overlay_color_entry->fg < 0 || overlay_color_entry->bg < 0))
  {
    source_color_entry = find_color_list_entry_by_pair (source.pair);
    if (source_color_entry)
    {
      merged_fg = overlay_color_entry->fg < 0 ?
        source_color_entry->fg :
        overlay_color_entry->fg;
      merged_bg = overlay_color_entry->bg < 0 ?
        source_color_entry->bg :
        overlay_color_entry->bg;
      merged.pair = mutt_alloc_overlay_color (merged_fg, merged_bg);
    }
  }
#endif /* HAVE_COLOR */

  merged.attrs = source.attrs | overlay.attrs;

  return merged;
}

void mutt_attrset_cursor (COLOR_ATTR source, COLOR_ATTR cursor)
{
  COLOR_ATTR merged = cursor;

  if (option (OPTCURSOROVERLAY))
    merged = mutt_merge_colors (source, cursor);

  ATTRSET (merged);
}

#ifdef HAVE_COLOR

static int _mutt_alloc_color (int fg, int bg, int type)
{
  COLOR_LIST *p, **last;
  int pair;

#if defined (USE_SLANG_CURSES)
  char fgc[SHORT_STRING], bgc[SHORT_STRING];
#endif

  /* Check to see if this color is already allocated to save space.
   *
   * At the same time, find the lowest available index, and location
   * in the list to store a new entry. The ColorList is sorted by
   * pair.  "last" points to &(previousentry->next), giving us the
   * slot to store it in.
   */
  pair = 1;
  last = &ColorList;
  p = *last;

  while (p)
  {
    if (p->fg == fg && p->bg == bg)
    {
      if (type == MUTT_COLOR_TYPE_ANSI)
      {
        if (!p->ansi)
        {
          p->ansi = 1;
          AnsiColors++;
        }
      }
      else if (type == MUTT_COLOR_TYPE_OVERLAY)
        p->overlay = 1;
      else
        (p->count)++;

      return p->pair;
    }

    if (p->pair <= pair)
    {
      last = &p->next;
      pair = p->pair + 1;
    }

    p = p->next;
  }

  /* check to see if there are colors left.
   * note: pair 0 is reserved for "default" so we actually only have access
   * to COLOR_PAIRS-1 pairs. */
  if (UserColors >= (COLOR_PAIRS - 1))
    return (0);

  /* Check for pair overflow too.  We are currently using init_pair(), which
   * only accepts size short. */
  if ((pair > SHRT_MAX) || (pair < 0))
    return (0);

  UserColors++;

  p = (COLOR_LIST *) safe_calloc (1, sizeof (COLOR_LIST));
  p->next = *last;
  *last = p;

  p->pair = pair;
  p->bg = bg;
  p->fg = fg;
  if (type == MUTT_COLOR_TYPE_ANSI)
  {
    p->ansi = 1;
    AnsiColors++;
  }
  else if (type == MUTT_COLOR_TYPE_OVERLAY)
    p->overlay = 1;
  else
    p->count = 1;

#if defined (USE_SLANG_CURSES)
  if (fg == COLOR_DEFAULT || bg == COLOR_DEFAULT)
  {
    SLtt_set_color (pair, NULL,
                    get_color_name (fgc, sizeof (fgc), fg),
                    get_color_name (bgc, sizeof (bgc), bg));
  }
  else
#endif

  /* NOTE: this may be the "else" clause of the SLANG #if block above. */
  {
    init_pair (p->pair, fg, bg);
  }

  dprint (3, (debugfile,"mutt_alloc_color(): Color pairs used so far: %d\n",
	      UserColors));

  return p->pair;
}

int mutt_alloc_color (int fg, int bg)
{
  return _mutt_alloc_color (fg, bg, MUTT_COLOR_TYPE_NORMAL);
}

int mutt_alloc_ansi_color (int fg, int bg)
{
  if (fg == COLOR_DEFAULT || bg == COLOR_DEFAULT)
  {
#if defined (HAVE_USE_DEFAULT_COLORS)
    if (!DefaultColorsInit)
      init_default_colors ();

    if (!HaveDefaultColors)
      return 0;
#elif !defined (USE_SLANG_CURSES)
    return 0;
#endif
  }

  return _mutt_alloc_color (fg, bg, MUTT_COLOR_TYPE_ANSI);
}

int mutt_alloc_overlay_color (int fg, int bg)
{
  return _mutt_alloc_color (fg, bg, MUTT_COLOR_TYPE_OVERLAY);
}


/* This is used to delete NORMAL type colors only.
 * Overlay colors are currently allowed to accumulate.
 * Ansi colors are deleted all at once, upon exiting the pager.
 */
static void mutt_free_color (int pair)
{
  COLOR_LIST *p, **last;

  last = &ColorList;
  p = *last;

  while (p)
  {
    if (p->pair == pair)
    {
      (p->count)--;

      if (p->count > 0 || p->ansi || p->overlay)
        return;

      UserColors--;
      dprint(1,(debugfile,"mutt_free_color(): Color pairs used so far: %d\n",
                UserColors));

      *last = p->next;
      FREE (&p);
      return;
    }
    if (p->pair > pair)
      return;

    last = &p->next;
    p = *last;
  }
}

void mutt_free_all_ansi_colors (void)
{
  COLOR_LIST *p, **last;

  last = &ColorList;
  p = *last;

  while (p && AnsiColors > 0)
  {
    if (p->ansi)
    {
      p->ansi = 0;
      AnsiColors--;

      if (!p->count && !p->overlay)
      {
        UserColors--;

        *last = p->next;
        FREE (&p);
        p = *last;
        continue;
      }
    }

    last = &p->next;
    p = *last;
  }
}

#endif /* HAVE_COLOR */


#ifdef HAVE_COLOR

static int
parse_color_name (const char *s, int *col, int *attr, int is_fg, BUFFER *err)
{
  char *eptr;
  int is_bright = 0, is_light = 0;

  if (ascii_strncasecmp (s, "bright", 6) == 0)
  {
    is_bright = 1;
    s += 6;
  }
  else if (ascii_strncasecmp (s, "light", 5) == 0)
  {
    is_light = 1;
    s += 5;
  }

  /* allow aliases for xterm color resources */
  if (ascii_strncasecmp (s, "color", 5) == 0)
  {
    s += 5;
    *col = strtol (s, &eptr, 10);
    if (!*s || *eptr || *col < 0 ||
	(*col >= COLORS && !option(OPTNOCURSES) && has_colors()))
    {
      snprintf (err->data, err->dsize, _("%s: color not supported by term"), s);
      return (-1);
    }
  }
  else
  {
    /* Note: mutt_getvaluebyname() returns -1 for "not found".
     * Since COLOR_DEFAULT is -1, we need to use this function instead. */
    const struct mapping_t *entry = mutt_get_mapentry_by_name (s, Colors);
    if (!entry)
    {
      snprintf (err->data, err->dsize, _("%s: no such color"), s);
      return (-1);
    }
    *col = entry->value;
  }

  if (is_bright || is_light)
  {
    if (is_fg)
    {
      if ((COLORS >= 16) && is_light)
      {
        if (*col >= 0 && *col <= 7)
        {
          /* Advance the color 0-7 by 8 to get the light version */
          *col += 8;
        }
      }
      else
      {
        *attr |= A_BOLD;
      }
    }
    else
    {
      if (COLORS >= 16)
      {
        if (*col >= 0 && *col <= 7)
        {
          /* Advance the color 0-7 by 8 to get the light version */
          *col += 8;
        }
      }
    }
  }

  return 0;
}

#endif


/* usage: uncolor index pattern [pattern...]
 * 	  unmono  index pattern [pattern...]
 */

static int
_mutt_parse_uncolor (BUFFER *buf, BUFFER *s, BUFFER *err, short parse_uncolor);


#ifdef HAVE_COLOR

int mutt_parse_uncolor (BUFFER *buf, BUFFER *s, union pointer_long_t udata,
			BUFFER *err)
{
  return _mutt_parse_uncolor(buf, s, err, 1);
}

#endif

int mutt_parse_unmono (BUFFER *buf, BUFFER *s, union pointer_long_t udata,
		       BUFFER *err)
{
  return _mutt_parse_uncolor(buf, s, err, 0);
}

static int _mutt_parse_uncolor (BUFFER *buf, BUFFER *s, BUFFER *err, short parse_uncolor)
{
  int object = 0, is_index = 0, do_cache = 0;
  COLOR_LINE *tmp, *last = NULL;
  COLOR_LINE **list;

  mutt_extract_token (buf, s, 0);

  if ((object = mutt_getvaluebyname (buf->data, Fields)) == -1)
  {
    snprintf (err->data, err->dsize, _("%s: no such object"), buf->data);
    return (-1);
  }

  if (object == MT_COLOR_INDEX)
  {
    is_index = 1;
    list = &ColorIndexList;
  }
  else if (object == MT_COLOR_BODY)
    list = &ColorBodyList;
  else if (object == MT_COLOR_HEADER)
    list = &ColorHdrList;
  else
  {
    snprintf (err->data, err->dsize,
	      _("%s: command valid only for index, body, header objects"),
	      parse_uncolor ? "uncolor" : "unmono");
    return (-1);
  }

  if (!MoreArgs (s))
  {
    snprintf (err->data, err->dsize,
	      _("%s: too few arguments"), parse_uncolor ? "uncolor" : "unmono");
    return (-1);
  }

  if (
#ifdef HAVE_COLOR
    /* we're running without curses */
    option (OPTNOCURSES)
    || /* we're parsing an uncolor command, and have no colors */
    (parse_uncolor && !has_colors())
    /* we're parsing an unmono command, and have colors */
    || (!parse_uncolor && has_colors())
#else
    /* We don't even have colors compiled in */
    parse_uncolor
#endif
    )
  {
    /* just eat the command, but don't do anything real about it */
    do
      mutt_extract_token (buf, s, 0);
    while (MoreArgs (s));

    return 0;
  }

  do
  {
    mutt_extract_token (buf, s, 0);
    if (!mutt_strcmp ("*", buf->data))
    {
      for (tmp = *list; tmp; )
      {
        if (!do_cache)
	  do_cache = 1;
	last = tmp;
	tmp = tmp->next;
	mutt_free_color_line(&last, parse_uncolor);
      }
      *list = NULL;
    }
    else
    {
      for (last = NULL, tmp = *list; tmp; last = tmp, tmp = tmp->next)
      {
	if (!mutt_strcmp (buf->data, tmp->pattern))
	{
          if (!do_cache)
	    do_cache = 1;
	  dprint(1,(debugfile,"Freeing pattern \"%s\" from color list\n",
                    tmp->pattern));
	  if (last)
	    last->next = tmp->next;
	  else
	    *list = tmp->next;
	  mutt_free_color_line(&tmp, parse_uncolor);
	  break;
	}
      }
    }
  }
  while (MoreArgs (s));


  if (is_index && do_cache && !option (OPTNOCURSES))
  {
    int i;
    mutt_set_menu_redraw_full (MENU_MAIN);
    /* force re-caching of index colors */
    for (i = 0; Context && i < Context->msgcount; i++)
    {
      Context->hdrs[i]->color.pair = 0;
      Context->hdrs[i]->color.attrs = 0;
    }
  }
  return (0);
}


static int
add_pattern (COLOR_LINE **top, const char *s, int sensitive,
	     int fg, int bg, int attr, BUFFER *err,
	     int is_index)
{

  /* is_index used to store compiled pattern
   * only for `index' color object
   * when called from mutt_parse_color() */

  COLOR_LINE *tmp = *top;

  while (tmp)
  {
    if (sensitive)
    {
      if (mutt_strcmp (s, tmp->pattern) == 0)
	break;
    }
    else
    {
      if (mutt_strcasecmp (s, tmp->pattern) == 0)
	break;
    }
    tmp = tmp->next;
  }

  if (tmp)
  {
#ifdef HAVE_COLOR
    if (fg != COLOR_UNSET && bg != COLOR_UNSET)
    {
      if (tmp->fg != fg || tmp->bg != bg)
      {
	mutt_free_color (tmp->color.pair);
	tmp->fg = fg;
	tmp->bg = bg;
        tmp->color.pair = mutt_alloc_color (fg, bg);
      }
      else
	attr |= (tmp->color.attrs & ~A_BOLD);
    }
#endif /* HAVE_COLOR */
    tmp->color.attrs = attr;
  }
  else
  {
    int r;
    BUFFER *buf = NULL;

    tmp = mutt_new_color_line ();
    if (is_index)
    {
      buf = mutt_buffer_pool_get ();
      mutt_buffer_strcpy(buf, NONULL(s));
      mutt_check_simple (buf, NONULL(SimpleSearch));
      tmp->color_pattern = mutt_pattern_comp (buf->data, MUTT_FULL_MSG, err);
      mutt_buffer_pool_release (&buf);
      if (tmp->color_pattern == NULL)
      {
	mutt_free_color_line(&tmp, 1);
	return -1;
      }
    }
    else if ((r = REGCOMP (&tmp->rx, s, (sensitive ? mutt_which_case (s) : REG_ICASE))) != 0)
    {
      regerror (r, &tmp->rx, err->data, err->dsize);
      mutt_free_color_line(&tmp, 1);
      return (-1);
    }
    tmp->next = *top;
    tmp->pattern = safe_strdup (s);
#ifdef HAVE_COLOR
    if (fg != COLOR_UNSET && bg != COLOR_UNSET)
    {
      tmp->fg = fg;
      tmp->bg = bg;
      tmp->color.pair = mutt_alloc_color (fg, bg);
    }
#endif
    tmp->color.attrs = attr;
    *top = tmp;
  }

  /* force re-caching of index colors */
  if (is_index)
  {
    int i;

    for (i = 0; Context && i < Context->msgcount; i++)
    {
      Context->hdrs[i]->color.pair = 0;
      Context->hdrs[i]->color.attrs = 0;
    }
  }

  return 0;
}

static int
parse_object(BUFFER *buf, BUFFER *s, int *o, int *ql, BUFFER *err)
{
  int q_level = 0;
  char *eptr;

  if (!MoreArgs(s))
  {
    strfcpy(err->data, _("Missing arguments."), err->dsize);
    return -1;
  }

  mutt_extract_token(buf, s, 0);
  if (!ascii_strncasecmp(buf->data, "quoted", 6))
  {
    if (buf->data[6])
    {
      *ql = strtol(buf->data + 6, &eptr, 10);
      if (*eptr || q_level < 0)
      {
	snprintf(err->data, err->dsize, _("%s: no such object"), buf->data);
	return -1;
      }
    }
    else
      *ql = 0;

    *o = MT_COLOR_QUOTED;
  }
  else if (!ascii_strcasecmp(buf->data, "compose"))
  {
    if (!MoreArgs(s))
    {
      strfcpy(err->data, _("Missing arguments."), err->dsize);
      return -1;
    }

    mutt_extract_token(buf, s, 0);

    if ((*o = mutt_getvaluebyname (buf->data, ComposeFields)) == -1)
    {
      snprintf (err->data, err->dsize, _("%s: no such object"), buf->data);
      return (-1);
    }
  }
  else if ((*o = mutt_getvaluebyname (buf->data, Fields)) == -1)
  {
    snprintf (err->data, err->dsize, _("%s: no such object"), buf->data);
    return (-1);
  }

  return 0;
}

typedef int (*parser_callback_t)(BUFFER *, BUFFER *, int *, int *, int *, BUFFER *);

#ifdef HAVE_COLOR

static int
parse_color_pair(BUFFER *buf, BUFFER *s, int *fg, int *bg, int *attr, BUFFER *err)
{
  FOREVER
  {
    if (! MoreArgs (s))
    {
      strfcpy (err->data, _("color: too few arguments"), err->dsize);
      return (-1);
    }

    mutt_extract_token (buf, s, 0);

    if (ascii_strcasecmp ("bold", buf->data) == 0)
      *attr |= A_BOLD;
    else if (ascii_strcasecmp ("underline", buf->data) == 0)
      *attr |= A_UNDERLINE;
    else if (ascii_strcasecmp ("none", buf->data) == 0)
      *attr = A_NORMAL;
    else if (ascii_strcasecmp ("reverse", buf->data) == 0)
      *attr |= A_REVERSE;
    else if (ascii_strcasecmp ("standout", buf->data) == 0)
      *attr |= A_STANDOUT;
    else if (ascii_strcasecmp ("normal", buf->data) == 0)
      *attr = A_NORMAL; /* needs use = instead of |= to clear other bits */
    else
    {
      if (parse_color_name (buf->data, fg, attr, 1, err) != 0)
        return (-1);
      break;
    }
  }

  if (! MoreArgs (s))
  {
    strfcpy (err->data, _("color: too few arguments"), err->dsize);
    return (-1);
  }

  mutt_extract_token (buf, s, 0);

  if (parse_color_name (buf->data, bg, attr, 0, err) != 0)
    return (-1);

  return 0;
}

#endif

static int
parse_attr_spec(BUFFER *buf, BUFFER *s, int *fg, int *bg, int *attr, BUFFER *err)
{

  if (fg) *fg = COLOR_UNSET;
  if (bg) *bg = COLOR_UNSET;

  if (! MoreArgs (s))
  {
    strfcpy (err->data, _("mono: too few arguments"), err->dsize);
    return (-1);
  }

  mutt_extract_token (buf, s, 0);

  if (ascii_strcasecmp ("bold", buf->data) == 0)
    *attr |= A_BOLD;
  else if (ascii_strcasecmp ("underline", buf->data) == 0)
    *attr |= A_UNDERLINE;
  else if (ascii_strcasecmp ("none", buf->data) == 0)
    *attr = A_NORMAL;
  else if (ascii_strcasecmp ("reverse", buf->data) == 0)
    *attr |= A_REVERSE;
  else if (ascii_strcasecmp ("standout", buf->data) == 0)
    *attr |= A_STANDOUT;
  else if (ascii_strcasecmp ("normal", buf->data) == 0)
    *attr = A_NORMAL; /* needs use = instead of |= to clear other bits */
  else
  {
    snprintf (err->data, err->dsize, _("%s: no such attribute"), buf->data);
    return (-1);
  }

  return 0;
}

static COLOR_ATTR fgbgattr_to_color (int fg, int bg, int attr)
{
  COLOR_ATTR color_attr = {0};
#ifdef HAVE_COLOR
  if (fg != COLOR_UNSET && bg != COLOR_UNSET)
    color_attr.pair = mutt_alloc_color (fg, bg);
#endif
  color_attr.attrs = attr;
  return color_attr;
}

/* usage: color <object> <fg> <bg> [ <regexp> ]
 * 	  mono  <object> <attr> [ <regexp> ]
 */

static int
_mutt_parse_color (BUFFER *buf, BUFFER *s, BUFFER *err,
		   parser_callback_t callback, short dry_run)
{
  int object = 0, attr = 0, fg = 0, bg = 0, q_level = 0;
  int r = 0;

  if (parse_object(buf, s, &object, &q_level, err) == -1)
    return -1;

  if (callback(buf, s, &fg, &bg, &attr, err) == -1)
    return -1;

  /* extract a regular expression if needed */

  if (object == MT_COLOR_HEADER || object == MT_COLOR_BODY || object == MT_COLOR_INDEX)
  {
    if (!MoreArgs (s))
    {
      strfcpy (err->data, _("too few arguments"), err->dsize);
      return (-1);
    }

    mutt_extract_token (buf, s, 0);
  }

  if (MoreArgs (s))
  {
    strfcpy (err->data, _("too many arguments"), err->dsize);
    return (-1);
  }

  /* dry run? */

  if (dry_run) return 0;


#if defined(HAVE_COLOR) && defined(HAVE_USE_DEFAULT_COLORS)
  if (!option (OPTNOCURSES) &&
      has_colors() &&
      (fg == COLOR_DEFAULT || bg == COLOR_DEFAULT))
  {
    /* delay use_default_colors() until needed, since it initializes things */
    if (!DefaultColorsInit)
      init_default_colors ();
    if (!HaveDefaultColors)
    {
      strfcpy (err->data, _("default colors not supported"), err->dsize);
      return (-1);
    }
  }
#endif  /* defined(HAVE_COLOR) && defined(HAVE_USE_DEFAULT_COLORS) */

  if (object == MT_COLOR_HEADER)
    r = add_pattern (&ColorHdrList, buf->data, 0, fg, bg, attr, err,0);
  else if (object == MT_COLOR_BODY)
    r = add_pattern (&ColorBodyList, buf->data, 1, fg, bg, attr, err, 0);
  else if (object == MT_COLOR_INDEX)
  {
    r = add_pattern (&ColorIndexList, buf->data, 1, fg, bg, attr, err, 1);
    mutt_set_menu_redraw_full (MENU_MAIN);
  }
  else if (object == MT_COLOR_QUOTED)
  {
    if (q_level >= ColorQuoteSize)
    {
      safe_realloc (&ColorQuote, (ColorQuoteSize += 2) * sizeof (COLOR_ATTR));
      ColorQuote[ColorQuoteSize-2] = ColorDefs[MT_COLOR_QUOTED];
      ColorQuote[ColorQuoteSize-1] = ColorDefs[MT_COLOR_QUOTED];
    }
    if (q_level >= ColorQuoteUsed)
      ColorQuoteUsed = q_level + 1;
    if (q_level == 0)
    {
      ColorDefs[MT_COLOR_QUOTED] = fgbgattr_to_color(fg, bg, attr);

      ColorQuote[0] = ColorDefs[MT_COLOR_QUOTED];
      for (q_level = 1; q_level < ColorQuoteUsed; q_level++)
      {
	if (ColorQuote[q_level].pair == 0 && ColorQuote[q_level].attrs == 0)
	  ColorQuote[q_level] = ColorDefs[MT_COLOR_QUOTED];
      }
    }
    else
      ColorQuote[q_level] = fgbgattr_to_color(fg, bg, attr);
  }
  else
    ColorDefs[object] = fgbgattr_to_color(fg, bg, attr);

  return (r);
}

#ifdef HAVE_COLOR

int mutt_parse_color(BUFFER *buff, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int dry_run = 0;

  if (option(OPTNOCURSES) || !has_colors())
    dry_run = 1;

  return _mutt_parse_color(buff, s, err, parse_color_pair, dry_run);
}

#endif

int mutt_parse_mono(BUFFER *buff, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int dry_run = 0;

#ifdef HAVE_COLOR
  if (option(OPTNOCURSES) || has_colors())
    dry_run = 1;
#else
  if (option(OPTNOCURSES))
    dry_run = 1;
#endif

  return _mutt_parse_color(buff, s, err, parse_attr_spec, dry_run);
}
