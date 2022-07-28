/*
 * Copyright (C) 1996-2000,2003,2013 Michael R. Elkins <me@mutt.org>
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
#include "mutt_menu.h"
#include "mutt_idna.h"
#include "mapping.h"
#include "send.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

typedef struct query
{
  int num;
  ADDRESS *addr;
  char *name;
  char *other;
  struct query *next;
} QUERY;

typedef struct entry
{
  int tagged;
  QUERY *data;
} ENTRY;

static const struct mapping_t QueryHelp[] = {
  { N_("Exit"),   OP_EXIT },
  { N_("Mail"),   OP_MAIL },
  { N_("New Query"),  OP_QUERY },
  { N_("Make Alias"), OP_CREATE_ALIAS },
  { N_("Search"), OP_SEARCH },
  { N_("Help"),   OP_HELP },
  { NULL,	  0 }
};

static void query_menu (char *buf, size_t buflen, QUERY *results, int retbuf);

static ADDRESS *result_to_addr (QUERY *r)
{
  static ADDRESS *tmp;

  if (!(tmp = rfc822_cpy_adr (r->addr, 0)))
    return NULL;

  if (!tmp->next && !tmp->personal)
  {
    tmp->personal = safe_strdup (r->name);
#ifdef EXACT_ADDRESS
    FREE (&tmp->val);
#endif
  }

  mutt_addrlist_to_intl (tmp, NULL);
  return tmp;
}

static void free_query (QUERY **query)
{
  QUERY *p;

  if (!query)
    return;

  while (*query)
  {
    p = *query;
    *query = (*query)->next;

    rfc822_free_address (&p->addr);
    FREE (&p->name);
    FREE (&p->other);
    FREE (&p);
  }
}

static QUERY *run_query (char *s, int quiet)
{
  FILE *fp;
  QUERY *first = NULL;
  QUERY *cur = NULL;
  BUFFER *cmd = NULL;
  char *buf = NULL;
  size_t buflen;
  int dummy = 0;
  char *msg = NULL;
  size_t msglen;
  char *tok, *nexttok;
  pid_t thepid;

  cmd = mutt_buffer_pool_get ();
  mutt_expand_file_fmt (cmd, QueryCmd, s);

  if ((thepid = mutt_create_filter (mutt_b2s (cmd), NULL, &fp, NULL)) < 0)
  {
    dprint (1, (debugfile, "unable to fork command: %s", mutt_b2s (cmd)));
    mutt_buffer_pool_release (&cmd);
    return 0;
  }
  mutt_buffer_pool_release (&cmd);

  if (!quiet)
    mutt_message _("Waiting for response...");

  /* The query protocol first reads one NL-terminated line. If an error
   * occurs, this is assumed to be an error message. Otherwise it's ignored. */
  msg = mutt_read_line (msg, &msglen, fp, &dummy, 0);
  while ((buf = mutt_read_line (buf, &buflen, fp, &dummy, 0)) != NULL)
  {
    tok = buf;
    if ((nexttok = strchr (tok, '\t')))
      *nexttok++ = 0;

    if (first == NULL)
    {
      first = (QUERY *) safe_calloc (1, sizeof (QUERY));
      cur = first;
    }
    else
    {
      cur->next = (QUERY *) safe_calloc (1, sizeof (QUERY));
      cur = cur->next;
    }

    cur->addr = rfc822_parse_adrlist (cur->addr, tok);
    if (nexttok)
    {
      tok = nexttok;
      if ((nexttok = strchr (tok, '\t')))
        *nexttok++ = 0;
      cur->name = safe_strdup (tok);

      if (nexttok)
      {
        tok = nexttok;
        if ((nexttok = strchr (tok, '\t')))
          *nexttok++ = 0;
        cur->other = safe_strdup (tok);
      }
    }
  }
  FREE (&buf);
  safe_fclose (&fp);
  if (mutt_wait_filter (thepid))
  {
    dprint (1, (debugfile, "Error: %s\n", msg));
    if (!quiet)  mutt_error ("%s", msg);
  }
  else
  {
    if (!quiet)
      mutt_message ("%s", msg);
  }
  FREE (&msg);

  return first;
}

static int query_search (MUTTMENU *m, regex_t *re, int n)
{
  ENTRY *table = (ENTRY *) m->data;

  if (table[n].data->name && !regexec (re, table[n].data->name, 0, NULL, 0))
    return 0;
  if (table[n].data->other && !regexec (re, table[n].data->other, 0, NULL, 0))
    return 0;
  if (table[n].data->addr)
  {
    if (table[n].data->addr->personal &&
	!regexec (re, table[n].data->addr->personal, 0, NULL, 0))
      return 0;
    if (table[n].data->addr->mailbox &&
	!regexec (re, table[n].data->addr->mailbox, 0, NULL, 0))
      return 0;
#ifdef EXACT_ADDRESS
    if (table[n].data->addr->val &&
	!regexec (re, table[n].data->addr->val, 0, NULL, 0))
      return 0;
#endif
  }

  return REG_NOMATCH;
}

static const char * query_format_str (char *dest, size_t destlen, size_t col, int cols,
				      char op, const char *src,
				      const char *fmt, const char *ifstring,
				      const char *elsestring,
				      void *data, format_flag flags)
{
  ENTRY *entry = (ENTRY *)data;
  QUERY *query = entry->data;
  char tmp[SHORT_STRING];
  char buf2[STRING] = "";
  int optional = (flags & MUTT_FORMAT_OPTIONAL);

  switch (op)
  {
    case 'a':
      rfc822_write_address (buf2, sizeof (buf2), query->addr, 1);
      mutt_format_s (dest, destlen, fmt, buf2);
      break;
    case 'c':
      snprintf (tmp, sizeof (tmp), "%%%sd", fmt);
      snprintf (dest, destlen, tmp, query->num + 1);
      break;
    case 'e':
      if (!optional)
        mutt_format_s (dest, destlen, fmt, NONULL (query->other));
      else if (!query->other || !*query->other)
        optional = 0;
      break;
    case 'n':
      mutt_format_s (dest, destlen, fmt, NONULL (query->name));
      break;
    case 't':
      snprintf (tmp, sizeof (tmp), "%%%sc", fmt);
      snprintf (dest, destlen, tmp, entry->tagged ? '*' : ' ');
      break;
    default:
      snprintf (tmp, sizeof (tmp), "%%%sc", fmt);
      snprintf (dest, destlen, tmp, op);
      break;
  }

  if (optional)
    mutt_FormatString (dest, destlen, col, cols, ifstring, query_format_str, data, 0);
  else if (flags & MUTT_FORMAT_OPTIONAL)
    mutt_FormatString (dest, destlen, col, cols, elsestring, query_format_str, data, 0);

  return src;
}

static void query_entry (char *s, size_t slen, MUTTMENU *m, int num)
{
  ENTRY *entry = &((ENTRY *) m->data)[num];

  entry->data->num = num;
  mutt_FormatString (s, slen, 0, MuttIndexWindow->cols, NONULL (QueryFormat), query_format_str,
		     entry, MUTT_FORMAT_ARROWCURSOR);
}

static int query_tag (MUTTMENU *menu, int n, int m)
{
  ENTRY *cur = &((ENTRY *) menu->data)[n];
  int ot = cur->tagged;

  cur->tagged = m >= 0 ? m : !cur->tagged;
  return cur->tagged - ot;
}

int mutt_query_complete (char *buf, size_t buflen)
{
  QUERY *results = NULL;
  ADDRESS *tmpa;

  if (!QueryCmd)
  {
    mutt_error _("Query command not defined.");
    return 0;
  }

  results = run_query (buf, 1);
  if (results)
  {
    /* only one response? */
    if (results->next == NULL)
    {
      tmpa = result_to_addr (results);
      mutt_addrlist_to_local (tmpa);
      buf[0] = '\0';
      rfc822_write_address (buf, buflen, tmpa, 0);
      rfc822_free_address (&tmpa);
      free_query (&results);
      mutt_clear_error ();
      return (0);
    }
    /* multiple results, choose from query menu */
    query_menu (buf, buflen, results, 1);
  }
  return (0);
}

void mutt_query_menu (char *buf, size_t buflen)
{
  if (!QueryCmd)
  {
    mutt_error _("Query command not defined.");
    return;
  }

  if (buf == NULL)
  {
    char buffer[STRING] = "";

    query_menu (buffer, sizeof (buffer), NULL, 0);
  }
  else
  {
    query_menu (buf, buflen, NULL, 1);
  }
}

static void query_menu (char *buf, size_t buflen, QUERY *results, int retbuf)
{
  MUTTMENU *menu;
  HEADER *msg = NULL;
  ENTRY *QueryTable = NULL;
  QUERY *queryp = NULL;
  int i, done = 0;
  int op;
  char helpstr[LONG_STRING];
  char title[STRING];

  if (results == NULL)
  {
    /* Prompt for Query */
    if (mutt_get_field (_("Query: "), buf, buflen, 0) == 0 && buf[0])
    {
      results = run_query (buf, 0);
    }
  }

  if (results)
  {
    snprintf (title, sizeof (title), _("Query '%s'"), buf);

    menu = mutt_new_menu (MENU_QUERY);
    menu->make_entry = query_entry;
    menu->search = query_search;
    menu->tag = query_tag;
    menu->title = title;
    menu->help = mutt_compile_help (helpstr, sizeof (helpstr), MENU_QUERY, QueryHelp);
    mutt_push_current_menu (menu);

    /* count the number of results */
    for (queryp = results; queryp; queryp = queryp->next)
      menu->max++;

    menu->data = QueryTable = (ENTRY *) safe_calloc (menu->max, sizeof (ENTRY));

    for (i = 0, queryp = results; queryp; queryp = queryp->next, i++)
      QueryTable[i].data = queryp;

    while (!done)
    {
      switch ((op = mutt_menuLoop (menu)))
      {
	case OP_QUERY_APPEND:
	case OP_QUERY:
	  if (mutt_get_field (_("Query: "), buf, buflen, 0) == 0 && buf[0])
	  {
	    QUERY *newresults = NULL;

	    newresults = run_query (buf, 0);

	    menu->redraw = REDRAW_FULL;
	    if (newresults)
	    {
	      snprintf (title, sizeof (title), _("Query '%s'"), buf);

	      if (op == OP_QUERY)
	      {
                free_query (&results);
		results = newresults;
		FREE (&QueryTable);
	      }
	      else
	      {
		/* append */
		for (queryp = results; queryp->next; queryp = queryp->next);

		queryp->next = newresults;
	      }


	      menu->current = 0;
              mutt_pop_current_menu (menu);
	      mutt_menuDestroy (&menu);
	      menu = mutt_new_menu (MENU_QUERY);
	      menu->make_entry = query_entry;
	      menu->search = query_search;
	      menu->tag = query_tag;
	      menu->title = title;
	      menu->help = mutt_compile_help (helpstr, sizeof (helpstr), MENU_QUERY, QueryHelp);
              mutt_push_current_menu (menu);

	      /* count the number of results */
	      for (queryp = results; queryp; queryp = queryp->next)
		menu->max++;

	      if (op == OP_QUERY)
	      {
		menu->data = QueryTable =
		  (ENTRY *) safe_calloc (menu->max, sizeof (ENTRY));

		for (i = 0, queryp = results; queryp;
		     queryp = queryp->next, i++)
		  QueryTable[i].data = queryp;
	      }
	      else
	      {
		int clear = 0;

		/* append */
		safe_realloc (&QueryTable, menu->max * sizeof (ENTRY));

		menu->data = QueryTable;

		for (i = 0, queryp = results; queryp;
		     queryp = queryp->next, i++)
		{
		  /* once we hit new entries, clear/init the tag */
		  if (queryp == newresults)
		    clear = 1;

		  QueryTable[i].data = queryp;
		  if (clear)
		    QueryTable[i].tagged = 0;
		}
	      }
	    }
	  }
	  break;

	case OP_CREATE_ALIAS:
	  if (menu->tagprefix)
	  {
	    ADDRESS *naddr = NULL;

	    for (i = 0; i < menu->max; i++)
	      if (QueryTable[i].tagged)
	      {
		ADDRESS *a = result_to_addr(QueryTable[i].data);
		rfc822_append (&naddr, a, 0);
		rfc822_free_address (&a);
	      }

	    mutt_create_alias (NULL, naddr);
	  }
	  else
	  {
	    ADDRESS *a = result_to_addr(QueryTable[menu->current].data);
	    mutt_create_alias (NULL, a);
	    rfc822_free_address (&a);
	  }
	  break;

	case OP_GENERIC_SELECT_ENTRY:
	  if (retbuf)
	  {
	    done = 2;
	    break;
	  }
	  /* fall through */

	case OP_MAIL:
	  msg = mutt_new_header ();
	  msg->env = mutt_new_envelope ();
	  if (!menu->tagprefix)
	  {
	    msg->env->to = result_to_addr(QueryTable[menu->current].data);
	  }
	  else
	  {
	    for (i = 0; i < menu->max; i++)
	      if (QueryTable[i].tagged)
	      {
		ADDRESS *a = result_to_addr(QueryTable[i].data);
		rfc822_append (&msg->env->to, a, 0);
		rfc822_free_address (&a);
	      }
	  }
	  mutt_send_message (SENDBACKGROUNDEDIT, msg, NULL, Context, NULL);
	  menu->redraw = REDRAW_FULL;
	  break;

	case OP_EXIT:
	  done = 1;
	  break;
      }
    }

    /* if we need to return the selected entries */
    if (retbuf && (done == 2))
    {
      int tagged = 0;
      size_t curpos = 0;

      memset (buf, 0, buflen);

      /* check for tagged entries */
      for (i = 0; i < menu->max; i++)
      {
	if (QueryTable[i].tagged)
	{
	  if (curpos == 0)
	  {
	    ADDRESS *tmpa = result_to_addr (QueryTable[i].data);
	    mutt_addrlist_to_local (tmpa);
	    tagged = 1;
	    rfc822_write_address (buf, buflen, tmpa, 0);
	    curpos = mutt_strlen (buf);
	    rfc822_free_address (&tmpa);
	  }
	  else if (curpos + 2 < buflen)
	  {
	    ADDRESS *tmpa = result_to_addr (QueryTable[i].data);
	    mutt_addrlist_to_local (tmpa);
	    strcat (buf, ", ");	/* __STRCAT_CHECKED__ */
	    rfc822_write_address ((char *) buf + curpos + 2, buflen - curpos - 2,
				  tmpa, 0);
	    curpos = mutt_strlen (buf);
	    rfc822_free_address (&tmpa);
	  }
	}
      }
      /* then enter current message */
      if (!tagged)
      {
	ADDRESS *tmpa = result_to_addr (QueryTable[menu->current].data);
	mutt_addrlist_to_local (tmpa);
	rfc822_write_address (buf, buflen, tmpa, 0);
	rfc822_free_address (&tmpa);
      }

    }

    free_query (&results);
    FREE (&QueryTable);

    mutt_pop_current_menu (menu);
    mutt_menuDestroy (&menu);
  }
}
