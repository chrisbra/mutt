/*
 * Copyright (C) 1996-2002,2010,2013,2016 Michael R. Elkins <me@mutt.org>
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

#include "version.h"
#include "mutt.h"
#include "mapping.h"
#include "mutt_curses.h"
#include "mutt_menu.h"
#include "mutt_regex.h"
#include "history.h"
#include "keymap.h"
#include "mbyte.h"
#include "charset.h"
#include "mutt_crypt.h"
#include "mutt_idna.h"
#include "group.h"
#include "mutt_lisp.h"

#if defined(USE_SSL)
#include "mutt_ssl.h"
#endif

#include "mx.h"
#include "init.h"
#include "mailbox.h"

#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/utsname.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/time.h>

#define CHECK_PAGER                                     \
  if ((CurrentMenu == MENU_PAGER) && (idx >= 0) &&	\
      (MuttVars[idx].flags & R_RESORT))                 \
  {                                                     \
    snprintf (err->data, err->dsize, "%s",              \
              _("Not available in this menu."));        \
    return (-1);                                        \
  }

typedef struct myvar
{
  char *name;
  char *value;
  struct myvar* next;
} myvar_t;

static myvar_t* MyVars;

static int var_to_string (int idx, BUFFER *val);
static void escape_string_to_buffer (BUFFER *dst, const char *src);

static void myvar_set (const char* var, const char* val);
static const char* myvar_get (const char* var);
static void myvar_del (const char* var);

extern char **envlist;

static void toggle_quadoption (int opt)
{
  int n = opt/4;
  int b = (opt % 4) * 2;

  QuadOptions[n] ^= (1 << b);
}

void set_quadoption (int opt, int flag)
{
  int n = opt/4;
  int b = (opt % 4) * 2;

  QuadOptions[n] &= ~(0x3 << b);
  QuadOptions[n] |= (flag & 0x3) << b;
}

int quadoption (int opt)
{
  int n = opt/4;
  int b = (opt % 4) * 2;

  return (QuadOptions[n] >> b) & 0x3;
}

static const char *option_type_name (int opt, int type)
{
  int i;

  for (i = 0; MuttVars[i].option; i++)
    if (MuttVars[i].type == type &&
        MuttVars[i].data.l == opt)
      return MuttVars[i].option;
  return NULL;
}

static const char *quadoption_name (int opt)
{
  return option_type_name (opt, DT_QUAD);
}

static const char *boolean_name (int opt)
{
  return option_type_name (opt, DT_BOOL);
}

int query_quadoption (int opt, const char *prompt)
{
  int v = quadoption (opt);

  switch (v)
  {
    case MUTT_YES:
    case MUTT_NO:
      return (v);

    default:
      v = mutt_yesorno_with_help (prompt, (v == MUTT_ASKYES),
                                  quadoption_name (opt));
      mutt_window_clearline (MuttMessageWindow, 0);
      return (v);
  }

  /* not reached */
}

/* This is slightly different from query_quadoption(), which only
 * prompts when the quadoption is of type "ask-*".
 *
 * This function always prompts, but provides a help string listing
 * the boolean name as a reference.  It should be used when displaying
 * the mutt_yesorno() prompt depends on the setting of the boolean.
 */
int mutt_query_boolean (int opt, const char *prompt, int def)
{
  return mutt_yesorno_with_help (prompt, def,
                                 boolean_name (opt));
}

/* given the variable ``s'', return the index into the rc_vars array which
   matches, or -1 if the variable is not found.  */
static int mutt_option_index (char *s)
{
  int i;

  for (i = 0; MuttVars[i].option; i++)
    if (mutt_strcmp (s, MuttVars[i].option) == 0)
      return (MuttVars[i].type == DT_SYN ?  mutt_option_index ((char *) MuttVars[i].data.p) : i);
  return (-1);
}

int mutt_extract_token (BUFFER *dest, BUFFER *tok, int flags)
{
  char		ch;
  char		qc = 0; /* quote char */
  char		*pc;

  /* Some callers used to rely on the (bad) assumption that dest->data
   * would be non-NULL after calling this function.  Perhaps I've missed
   * a few cases, or a future caller might make the same mistake.
   */
  if (!dest->data)
    mutt_buffer_increase_size (dest, STRING);
  mutt_buffer_clear (dest);

  SKIPWS (tok->dptr);

  if ((*tok->dptr == '(') && !(flags & MUTT_TOKEN_NOLISP) &&
      ((flags & MUTT_TOKEN_LISP) || option (OPTMUTTLISPINLINEEVAL)))
  {
    int rc = mutt_lisp_eval_list (dest, tok);
    SKIPWS (tok->dptr);
    return rc;
  }

  while ((ch = *tok->dptr))
  {
    if (!qc)
    {
      if ((ISSPACE (ch) && !(flags & MUTT_TOKEN_SPACE)) ||
	  (ch == '#' && !(flags & MUTT_TOKEN_COMMENT)) ||
	  (ch == '=' && (flags & MUTT_TOKEN_EQUAL)) ||
	  (ch == ';' && !(flags & MUTT_TOKEN_SEMICOLON)) ||
	  ((flags & MUTT_TOKEN_PATTERN) && strchr ("~%=!|", ch)))
	break;
    }

    tok->dptr++;

    if (ch == qc)
      qc = 0; /* end of quote */
    else if (!qc && (ch == '\'' || ch == '"') && !(flags & MUTT_TOKEN_QUOTE))
      qc = ch;
    else if (ch == '\\' && qc != '\'')
    {
      if (!*tok->dptr)
        return -1; /* premature end of token */
      switch (ch = *tok->dptr++)
      {
	case 'c':
	case 'C':
          if (!*tok->dptr)
            return -1; /* premature end of token */
	  mutt_buffer_addch (dest, (toupper ((unsigned char) *tok->dptr)
                                    - '@') & 0x7f);
	  tok->dptr++;
	  break;
	case 'r':
	  mutt_buffer_addch (dest, '\r');
	  break;
	case 'n':
	  mutt_buffer_addch (dest, '\n');
	  break;
	case 't':
	  mutt_buffer_addch (dest, '\t');
	  break;
	case 'f':
	  mutt_buffer_addch (dest, '\f');
	  break;
	case 'e':
	  mutt_buffer_addch (dest, '\033');
	  break;
	default:
	  if (isdigit ((unsigned char) ch) &&
	      isdigit ((unsigned char) *tok->dptr) &&
	      isdigit ((unsigned char) *(tok->dptr + 1)))
	  {

	    mutt_buffer_addch (dest, (ch << 6) + (*tok->dptr << 3) + *(tok->dptr + 1) - 3504);
	    tok->dptr += 2;
	  }
	  else
	    mutt_buffer_addch (dest, ch);
      }
    }
    else if (ch == '^' && (flags & MUTT_TOKEN_CONDENSE))
    {
      if (!*tok->dptr)
        return -1; /* premature end of token */
      ch = *tok->dptr++;
      if (ch == '^')
	mutt_buffer_addch (dest, ch);
      else if (ch == '[')
	mutt_buffer_addch (dest, '\033');
      else if (isalpha ((unsigned char) ch))
	mutt_buffer_addch (dest, toupper ((unsigned char) ch) - '@');
      else
      {
	mutt_buffer_addch (dest, '^');
	mutt_buffer_addch (dest, ch);
      }
    }
    else if (ch == '`' && (!qc || qc == '"'))
    {
      FILE	*fp;
      pid_t	pid;
      char	*cmd;
      BUFFER	expn;
      int	line = 0, rc;

      pc = tok->dptr;
      do
      {
	if ((pc = strpbrk (pc, "\\`")))
	{
	  /* skip any quoted chars */
	  if (*pc == '\\')
          {
            if (*(pc+1))
              pc += 2;
            else
              pc = NULL;
          }
	}
      } while (pc && *pc != '`');
      if (!pc)
      {
	dprint (1, (debugfile, "mutt_get_token: mismatched backticks\n"));
	return (-1);
      }
      cmd = mutt_substrdup (tok->dptr, pc);
      if ((pid = mutt_create_filter (cmd, NULL, &fp, NULL)) < 0)
      {
	dprint (1, (debugfile, "mutt_get_token: unable to fork command: %s", cmd));
	FREE (&cmd);
	return (-1);
      }

      tok->dptr = pc + 1;

      /* read line */
      mutt_buffer_init (&expn);
      expn.data = mutt_read_line (NULL, &expn.dsize, fp, &line, 0);
      safe_fclose (&fp);
      rc = mutt_wait_filter (pid);
      if (rc != 0)
        dprint (1, (debugfile, "mutt_extract_token: backticks exited code %d for command: %s\n", rc, cmd));
      FREE (&cmd);

      /* If this is inside a quoted string, directly add output to
       * the token (dest) */
      if (expn.data && qc)
      {
	mutt_buffer_addstr (dest, expn.data);
      }
      /* Otherwise, reset tok to the shell output plus whatever else
       * was left on the original line and continue processing it. */
      else if (expn.data)
      {
        mutt_buffer_fix_dptr (&expn);
        mutt_buffer_addstr (&expn, tok->dptr);
        mutt_buffer_strcpy (tok, expn.data);
        mutt_buffer_rewind (tok);
      }

      FREE (&expn.data);
    }
    else if (ch == '$' && (!qc || qc == '"') && (*tok->dptr == '{' || isalpha ((unsigned char) *tok->dptr)))
    {
      const char *env = NULL;
      char *var = NULL;
      int idx;

      if (*tok->dptr == '{')
      {
	tok->dptr++;
	if ((pc = strchr (tok->dptr, '}')))
	{
	  var = mutt_substrdup (tok->dptr, pc);
	  tok->dptr = pc + 1;
	}
      }
      else
      {
	for (pc = tok->dptr; isalnum ((unsigned char) *pc) || *pc == '_'; pc++)
	  ;
	var = mutt_substrdup (tok->dptr, pc);
	tok->dptr = pc;
      }
      if (var)
      {
        if ((env = getenv (var)) || (env = myvar_get (var)))
          mutt_buffer_addstr (dest, env);
        else if ((idx = mutt_option_index (var)) != -1)
        {
          /* expand settable mutt variables */
          BUFFER *val = mutt_buffer_pool_get ();

          if (var_to_string (idx, val))
          {
            /* This flag is not used.  I'm keeping the code for the next
             * release cycle in case it needs to be reenabled for hooks.
             */
            if (flags & MUTT_TOKEN_ESC_VARS)
            {
              BUFFER *escval = mutt_buffer_pool_get ();

              escape_string_to_buffer (escval, mutt_b2s (val));
              mutt_buffer_addstr (dest, mutt_b2s (escval));
              mutt_buffer_pool_release (&escval);
            }
            else
              mutt_buffer_addstr (dest, mutt_b2s (val));
          }
          mutt_buffer_pool_release (&val);
        }
        FREE (&var);
      }
    }
    else
      mutt_buffer_addch (dest, ch);
  }
  SKIPWS (tok->dptr);
  return 0;
}

static void mutt_free_opt (struct option_t* p)
{
  REGEXP* pp;

  switch (p->type & DT_MASK)
  {
    case DT_ADDR:
      rfc822_free_address ((ADDRESS**)p->data.p);
      break;
    case DT_RX:
      pp = (REGEXP*)p->data.p;
      FREE (&pp->pattern);
      if (pp->rx)
      {
        regfree (pp->rx);
        FREE (&pp->rx);
      }
      break;
    case DT_PATH:
    case DT_CMD_PATH:
    case DT_STR:
      FREE ((char**)p->data.p);		/* __FREE_CHECKED__ */
      break;
  }
}

/* clean up before quitting */
void mutt_free_opts (void)
{
  int i;

  for (i = 0; MuttVars[i].option; i++)
    mutt_free_opt (MuttVars + i);

  mutt_free_rx_list (&Alternates);
  mutt_free_rx_list (&UnAlternates);
  mutt_free_rx_list (&MailLists);
  mutt_free_rx_list (&UnMailLists);
  mutt_free_rx_list (&SubscribedLists);
  mutt_free_rx_list (&UnSubscribedLists);
  mutt_free_rx_list (&NoSpamList);
}

static void add_to_list (LIST **list, const char *str)
{
  LIST *t, *last = NULL;

  /* don't add a NULL or empty string to the list */
  if (!str || *str == '\0')
    return;

  /* check to make sure the item is not already on this list */
  for (last = *list; last; last = last->next)
  {
    if (ascii_strcasecmp (str, last->data) == 0)
    {
      /* already on the list, so just ignore it */
      last = NULL;
      break;
    }
    if (!last->next)
      break;
  }

  if (!*list || last)
  {
    t = (LIST *) safe_calloc (1, sizeof (LIST));
    t->data = safe_strdup (str);
    if (last)
      last->next = t;
    else
      *list = t;
  }
}

int mutt_add_to_rx_list (RX_LIST **list, const char *s, int flags, BUFFER *err)
{
  RX_LIST *t, *last = NULL;
  REGEXP *rx;

  if (!s || !*s)
    return 0;

  if (!(rx = mutt_compile_regexp (s, flags)))
  {
    snprintf (err->data, err->dsize, "Bad regexp: %s\n", s);
    return -1;
  }

  /* check to make sure the item is not already on this list */
  for (last = *list; last; last = last->next)
  {
    if (ascii_strcasecmp (rx->pattern, last->rx->pattern) == 0)
    {
      /* already on the list, so just ignore it */
      last = NULL;
      break;
    }
    if (!last->next)
      break;
  }

  if (!*list || last)
  {
    t = mutt_new_rx_list();
    t->rx = rx;
    if (last)
      last->next = t;
    else
      *list = t;
  }
  else /* duplicate */
    mutt_free_regexp (&rx);

  return 0;
}

static int remove_from_replace_list (REPLACE_LIST **list, const char *pat);

static int add_to_replace_list (REPLACE_LIST **list, const char *pat, const char *templ, BUFFER *err)
{
  REPLACE_LIST *t = NULL, *last = NULL;
  REGEXP *rx;
  int n;
  const char *p;

  if (!pat || !*pat || !templ)
    return 0;

  if (!(rx = mutt_compile_regexp (pat, REG_ICASE)))
  {
    snprintf (err->data, err->dsize, _("Bad regexp: %s"), pat);
    return -1;
  }

  /* check to make sure the item is not already on this list */
  for (last = *list; last; last = last->next)
  {
    if (ascii_strcasecmp (rx->pattern, last->rx->pattern) == 0)
    {
      /* Already on the list. Formerly we just skipped this case, but
       * now we're supporting removals, which means we're supporting
       * re-adds conceptually. So we probably want this to imply a
       * removal, then do an add. We can achieve the removal by freeing
       * the template, and leaving t pointed at the current item.
       */
      t = last;
      FREE(&t->template);
      break;
    }
    if (!last->next)
      break;
  }

  /* If t is set, it's pointing into an extant REPLACE_LIST* that we want to
   * update. Otherwise we want to make a new one to link at the list's end.
   */
  if (!t)
  {
    t = mutt_new_replace_list();
    t->rx = rx;
    if (last)
      last->next = t;
    else
      *list = t;
  }

  /* Now t is the REPLACE_LIST* that we want to modify. It is prepared. */
  t->template = safe_strdup(templ);

  /* Find highest match number in template string */
  t->nmatch = 0;
  for (p = templ; *p;)
  {
    if (*p == '%')
    {
      n = atoi(++p);
      if (n > t->nmatch)
        t->nmatch = n;
      while (*p && isdigit((int)*p))
        ++p;
    }
    else
      ++p;
  }

  if (t->nmatch > t->rx->rx->re_nsub)
  {
    snprintf (err->data, err->dsize, "%s", _("Not enough subexpressions for "
                                             "template"));
    remove_from_replace_list(list, pat);
    return -1;
  }

  t->nmatch++;         /* match 0 is always the whole expr */

  return 0;
}

static int remove_from_replace_list (REPLACE_LIST **list, const char *pat)
{
  REPLACE_LIST *cur, *prev;
  int nremoved = 0;

  /* Being first is a special case. */
  cur = *list;
  if (!cur)
    return 0;
  if (cur->rx && !mutt_strcmp(cur->rx->pattern, pat))
  {
    *list = cur->next;
    mutt_free_regexp(&cur->rx);
    FREE(&cur->template);
    FREE(&cur);
    return 1;
  }

  prev = cur;
  for (cur = prev->next; cur;)
  {
    if (!mutt_strcmp(cur->rx->pattern, pat))
    {
      prev->next = cur->next;
      mutt_free_regexp(&cur->rx);
      FREE(&cur->template);
      FREE(&cur);
      cur = prev->next;
      ++nremoved;
    }
    else
      cur = cur->next;
  }

  return nremoved;
}


static void remove_from_list (LIST **l, const char *str)
{
  LIST *p, *last = NULL;

  if (mutt_strcmp ("*", str) == 0)
    mutt_free_list (l);    /* ``unCMD *'' means delete all current entries */
  else
  {
    p = *l;
    last = NULL;
    while (p)
    {
      if (ascii_strcasecmp (str, p->data) == 0)
      {
	FREE (&p->data);
	if (last)
	  last->next = p->next;
	else
	  (*l) = p->next;
	FREE (&p);
      }
      else
      {
	last = p;
	p = p->next;
      }
    }
  }
}

static void free_mbchar_table (mbchar_table **t)
{
  if (!t || !*t)
    return;

  FREE (&(*t)->chars);
  FREE (&(*t)->segmented_str);
  FREE (&(*t)->orig_str);
  FREE (t);		/* __FREE_CHECKED__ */
}

static mbchar_table *parse_mbchar_table (const char *s)
{
  mbchar_table *t;
  size_t slen, k;
  mbstate_t mbstate;
  char *d;

  t = safe_calloc (1, sizeof (mbchar_table));
  slen = mutt_strlen (s);
  if (!slen)
    return t;

  t->orig_str = safe_strdup (s);
  /* This could be more space efficient.  However, being used on tiny
   * strings (Tochars and StChars), the overhead is not great. */
  t->chars = safe_calloc (slen, sizeof (char *));
  d = t->segmented_str = safe_calloc (slen * 2, sizeof (char));

  memset (&mbstate, 0, sizeof (mbstate));
  while (slen && (k = mbrtowc (NULL, s, slen, &mbstate)))
  {
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      dprint (1, (debugfile,
                  "parse_mbchar_table: mbrtowc returned %d converting %s in %s\n",
                  (k == (size_t)(-1)) ? -1 : -2,
                  s, t->orig_str));
      if (k == (size_t)(-1))
        memset (&mbstate, 0, sizeof (mbstate));
      k = (k == (size_t)(-1)) ? 1 : slen;
    }

    slen -= k;
    t->chars[t->len++] = d;
    while (k--)
      *d++ = *s++;
    *d++ = '\0';
  }

  return t;
}

static int parse_unignore (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  do
  {
    mutt_extract_token (buf, s, 0);

    /* don't add "*" to the unignore list */
    if (strcmp (buf->data, "*"))
      add_to_list (&UnIgnore, buf->data);

    remove_from_list (&Ignore, buf->data);
  }
  while (MoreArgs (s));

  return 0;
}

static int parse_ignore (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  do
  {
    mutt_extract_token (buf, s, 0);
    remove_from_list (&UnIgnore, buf->data);
    add_to_list (&Ignore, buf->data);
  }
  while (MoreArgs (s));

  return 0;
}

static int parse_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  LIST **data = udata.p;
  do
  {
    mutt_extract_token (buf, s, 0);
    add_to_list (data, buf->data);
  }
  while (MoreArgs (s));

  return 0;
}

static int parse_echo (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  if (!MoreArgs (s))
  {
    strfcpy (err->data, _("not enough arguments"), err->dsize);
    return -1;
  }
  mutt_extract_token (buf, s, 0);
  set_option (OPTFORCEREFRESH);
  mutt_message ("%s", buf->data);
  unset_option (OPTFORCEREFRESH);
  mutt_sleep (0);

  return 0;
}

static void _alternates_clean (void)
{
  int i;
  if (Context && Context->msgcount)
  {
    for (i = 0; i < Context->msgcount; i++)
      Context->hdrs[i]->recip_valid = 0;
  }
}

static int parse_alternates (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  group_context_t *gc = NULL;

  _alternates_clean();

  do
  {
    mutt_extract_token (buf, s, 0);

    if (parse_group_context (&gc, buf, s, err) == -1)
      goto bail;

    mutt_remove_from_rx_list (&UnAlternates, buf->data);

    if (mutt_add_to_rx_list (&Alternates, buf->data, REG_ICASE, err) != 0)
      goto bail;

    if (mutt_group_context_add_rx (gc, buf->data, REG_ICASE, err) != 0)
      goto bail;
  }
  while (MoreArgs (s));

  mutt_group_context_destroy (&gc);
  return 0;

bail:
  mutt_group_context_destroy (&gc);
  return -1;
}

static int parse_unalternates (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  _alternates_clean();
  do
  {
    mutt_extract_token (buf, s, 0);
    mutt_remove_from_rx_list (&Alternates, buf->data);

    if (mutt_strcmp (buf->data, "*") &&
	mutt_add_to_rx_list (&UnAlternates, buf->data, REG_ICASE, err) != 0)
      return -1;

  }
  while (MoreArgs (s));

  return 0;
}

static int parse_replace_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  REPLACE_LIST **list = (REPLACE_LIST **)udata.p;
  BUFFER *templ = NULL;
  int rc = -1;

  /* First token is a regexp. */
  if (!MoreArgs(s))
  {
    strfcpy(err->data, _("not enough arguments"), err->dsize);
    return -1;
  }
  mutt_extract_token(buf, s, 0);

  /* Second token is a replacement template */
  if (!MoreArgs(s))
  {
    strfcpy(err->data, _("not enough arguments"), err->dsize);
    return -1;
  }

  templ = mutt_buffer_pool_get ();
  mutt_extract_token(templ, s, 0);
  if (add_to_replace_list(list, buf->data, mutt_b2s (templ), err) != 0)
    goto cleanup;

  rc = 0;

cleanup:
  mutt_buffer_pool_release (&templ);
  return rc;
}

static int parse_unreplace_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  REPLACE_LIST **list = (REPLACE_LIST **)udata.p;

  /* First token is a regexp. */
  if (!MoreArgs(s))
  {
    strfcpy(err->data, _("not enough arguments"), err->dsize);
    return -1;
  }

  mutt_extract_token(buf, s, 0);

  /* "*" is a special case. */
  if (!mutt_strcmp (buf->data, "*"))
  {
    mutt_free_replace_list (list);
    return 0;
  }

  remove_from_replace_list(list, buf->data);
  return 0;
}


static void clear_subject_mods (void)
{
  int i;
  if (Context && Context->msgcount)
  {
    for (i = 0; i < Context->msgcount; i++)
      FREE(&Context->hdrs[i]->env->disp_subj);
  }
}


static int parse_subjectrx_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int rc;

  rc = parse_replace_list(buf, s, udata, err);
  if (rc == 0)
    clear_subject_mods();
  return rc;
}


static int parse_unsubjectrx_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int rc;

  rc = parse_unreplace_list(buf, s, udata, err);
  if (rc == 0)
    clear_subject_mods();
  return rc;
}


static int parse_spam_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  long data = udata.l;

  /* Insist on at least one parameter */
  if (!MoreArgs(s))
  {
    if (data == MUTT_SPAM)
      strfcpy(err->data, _("spam: no matching pattern"), err->dsize);
    else
      strfcpy(err->data, _("nospam: no matching pattern"), err->dsize);
    return -1;
  }

  /* Extract the first token, a regexp */
  mutt_extract_token (buf, s, 0);

  /* data should be either MUTT_SPAM or MUTT_NOSPAM. MUTT_SPAM is for spam commands. */
  if (data == MUTT_SPAM)
  {
    /* If there's a second parameter, it's a template for the spam tag. */
    if (MoreArgs(s))
    {
      BUFFER *templ = NULL;

      templ = mutt_buffer_pool_get ();
      mutt_extract_token (templ, s, 0);

      /* Add to the spam list. */
      if (add_to_replace_list (&SpamList, buf->data, mutt_b2s (templ), err) != 0)
      {
        mutt_buffer_pool_release (&templ);
        return -1;
      }
      mutt_buffer_pool_release (&templ);
    }

    /* If not, try to remove from the nospam list. */
    else
    {
      mutt_remove_from_rx_list(&NoSpamList, buf->data);
    }

    return 0;
  }

  /* MUTT_NOSPAM is for nospam commands. */
  else if (data == MUTT_NOSPAM)
  {
    /* nospam only ever has one parameter. */

    /* "*" is a special case. */
    if (!mutt_strcmp(buf->data, "*"))
    {
      mutt_free_replace_list (&SpamList);
      mutt_free_rx_list (&NoSpamList);
      return 0;
    }

    /* If it's on the spam list, just remove it. */
    if (remove_from_replace_list(&SpamList, buf->data) != 0)
      return 0;

    /* Otherwise, add it to the nospam list. */
    if (mutt_add_to_rx_list (&NoSpamList, buf->data, REG_ICASE, err) != 0)
      return -1;

    return 0;
  }

  /* This should not happen. */
  strfcpy(err->data, "This is no good at all.", err->dsize);
  return -1;
}


static int parse_unlist (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  LIST **data = udata.p;
  do
  {
    mutt_extract_token (buf, s, 0);
    /*
     * Check for deletion of entire list
     */
    if (mutt_strcmp (buf->data, "*") == 0)
    {
      mutt_free_list (data);
      break;
    }
    remove_from_list (data, buf->data);
  }
  while (MoreArgs (s));

  return 0;
}

/* These two functions aren't sidebar-specific, but they are currently only
 * used by the sidebar_whitelist/unsidebar_whitelist commands.
 * Putting in an #ifdef to silence an unused function warning when the sidebar
 * is disabled.
 */
#ifdef USE_SIDEBAR
static int parse_path_list (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  BUFFER *path;
  LIST **data = udata.p;

  path = mutt_buffer_pool_get ();
  do
  {
    mutt_extract_token (path, s, 0);
    mutt_buffer_expand_path (path);
    add_to_list (data, mutt_b2s (path));
  }
  while (MoreArgs (s));
  mutt_buffer_pool_release (&path);

  return 0;
}

static int parse_path_unlist (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  BUFFER *path;
  LIST **data = udata.p;

  path = mutt_buffer_pool_get ();
  do
  {
    mutt_extract_token (path, s, 0);
    /*
     * Check for deletion of entire list
     */
    if (mutt_strcmp (mutt_b2s (path), "*") == 0)
    {
      mutt_free_list (data);
      break;
    }
    mutt_buffer_expand_path (path);
    remove_from_list (data, mutt_b2s (path));
  }
  while (MoreArgs (s));
  mutt_buffer_pool_release (&path);

  return 0;
}
#endif /* USE_SIDEBAR */

static int parse_lists (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  group_context_t *gc = NULL;

  do
  {
    mutt_extract_token (buf, s, 0);

    if (parse_group_context (&gc, buf, s, err) == -1)
      goto bail;

    mutt_remove_from_rx_list (&UnMailLists, buf->data);

    if (mutt_add_to_rx_list (&MailLists, buf->data, REG_ICASE, err) != 0)
      goto bail;

    if (mutt_group_context_add_rx (gc, buf->data, REG_ICASE, err) != 0)
      goto bail;
  }
  while (MoreArgs (s));

  mutt_group_context_destroy (&gc);
  return 0;

bail:
  mutt_group_context_destroy (&gc);
  return -1;
}

typedef enum group_state_t {
  NONE, RX, ADDR
} group_state_t;

static int parse_group (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  group_context_t *gc = NULL;
  group_state_t state = NONE;
  ADDRESS *addr = NULL;
  char *estr = NULL;
  long data = udata.l;

  do
  {
    mutt_extract_token (buf, s, 0);
    if (parse_group_context (&gc, buf, s, err) == -1)
      goto bail;

    if (data == MUTT_UNGROUP && !mutt_strcasecmp (buf->data, "*"))
    {
      if (mutt_group_context_clear (&gc) < 0)
	goto bail;
      goto out;
    }

    if (!mutt_strcasecmp (buf->data, "-rx"))
      state = RX;
    else if (!mutt_strcasecmp (buf->data, "-addr"))
      state = ADDR;
    else
    {
      switch (state)
      {
	case NONE:
	  snprintf (err->data, err->dsize, _("%sgroup: missing -rx or -addr."),
                    data == MUTT_UNGROUP ? "un" : "");
	  goto bail;

	case RX:
	  if (data == MUTT_GROUP &&
	      mutt_group_context_add_rx (gc, buf->data, REG_ICASE, err) != 0)
	    goto bail;
	  else if (data == MUTT_UNGROUP &&
		   mutt_group_context_remove_rx (gc, buf->data) < 0)
	    goto bail;
	  break;

	case ADDR:
	  if ((addr = mutt_parse_adrlist (NULL, buf->data)) == NULL)
	    goto bail;
	  if (mutt_addrlist_to_intl (addr, &estr))
	  {
	    snprintf (err->data, err->dsize, _("%sgroup: warning: bad IDN '%s'.\n"),
		      data == MUTT_UNGROUP ? "un" : "", estr);
            FREE (&estr);
            rfc822_free_address (&addr);
	    goto bail;
	  }
	  if (data == MUTT_GROUP)
	    mutt_group_context_add_adrlist (gc, addr);
	  else if (data == MUTT_UNGROUP)
	    mutt_group_context_remove_adrlist (gc, addr);
	  rfc822_free_address (&addr);
	  break;
      }
    }
  } while (MoreArgs (s));

out:
  mutt_group_context_destroy (&gc);
  return 0;

bail:
  mutt_group_context_destroy (&gc);
  return -1;
}

/* always wise to do what someone else did before */
static void _attachments_clean (void)
{
  int i;
  if (Context && Context->msgcount)
  {
    for (i = 0; i < Context->msgcount; i++)
      Context->hdrs[i]->attach_valid = 0;
  }
}

static int parse_attach_list (BUFFER *buf, BUFFER *s, LIST **ldata, BUFFER *err)
{
  ATTACH_MATCH *a;
  LIST *listp, *lastp;
  char *p;
  char *tmpminor;
  size_t len;
  int ret;

  /* Find the last item in the list that data points to. */
  lastp = NULL;
  dprint(5, (debugfile, "parse_attach_list: ldata = %p, *ldata = %p\n",
             (void *)ldata, (void *)*ldata));
  for (listp = *ldata; listp; listp = listp->next)
  {
    a = (ATTACH_MATCH *)listp->data;
    dprint(5, (debugfile, "parse_attach_list: skipping %s/%s\n",
               a->major, a->minor));
    lastp = listp;
  }

  do
  {
    mutt_extract_token (buf, s, 0);

    if (!buf->data || *buf->data == '\0')
      continue;

    a = safe_malloc(sizeof(ATTACH_MATCH));

    /* some cheap hacks that I expect to remove */
    if (!ascii_strcasecmp(buf->data, "any"))
      a->major = safe_strdup("*/.*");
    else if (!ascii_strcasecmp(buf->data, "none"))
      a->major = safe_strdup("cheap_hack/this_should_never_match");
    else
      a->major = safe_strdup(buf->data);

    if ((p = strchr(a->major, '/')))
    {
      *p = '\0';
      ++p;
      a->minor = p;
    }
    else
    {
      a->minor = "unknown";
    }

    len = strlen(a->minor);
    tmpminor = safe_malloc(len+3);
    strcpy(&tmpminor[1], a->minor); /* __STRCPY_CHECKED__ */
    tmpminor[0] = '^';
    tmpminor[len+1] = '$';
    tmpminor[len+2] = '\0';

    a->major_int = mutt_check_mime_type(a->major);
    ret = REGCOMP(&a->minor_rx, tmpminor, REG_ICASE);

    FREE(&tmpminor);

    if (ret)
    {
      regerror(ret, &a->minor_rx, err->data, err->dsize);
      FREE(&a->major);
      FREE(&a);
      return -1;
    }

    dprint(5, (debugfile, "parse_attach_list: added %s/%s [%d]\n",
               a->major, a->minor, a->major_int));

    listp = safe_malloc(sizeof(LIST));
    listp->data = (char *)a;
    listp->next = NULL;
    if (lastp)
    {
      lastp->next = listp;
    }
    else
    {
      *ldata = listp;
    }
    lastp = listp;
  }
  while (MoreArgs (s));

  _attachments_clean();
  return 0;
}

static int parse_unattach_list (BUFFER *buf, BUFFER *s, LIST **ldata, BUFFER *err)
{
  ATTACH_MATCH *a;
  LIST *lp, *lastp, *newlp;
  char *tmp;
  int major;
  char *minor;

  do
  {
    mutt_extract_token (buf, s, 0);

    if (!ascii_strcasecmp(buf->data, "any"))
      tmp = safe_strdup("*/.*");
    else if (!ascii_strcasecmp(buf->data, "none"))
      tmp = safe_strdup("cheap_hack/this_should_never_match");
    else
      tmp = safe_strdup(buf->data);

    if ((minor = strchr(tmp, '/')))
    {
      *minor = '\0';
      ++minor;
    }
    else
    {
      minor = "unknown";
    }
    major = mutt_check_mime_type(tmp);

    /* We must do our own walk here because remove_from_list() will only
     * remove the LIST->data, not anything pointed to by the LIST->data. */
    lastp = NULL;
    for (lp = *ldata; lp; )
    {
      a = (ATTACH_MATCH *)lp->data;
      dprint(5, (debugfile, "parse_unattach_list: check %s/%s [%d] : %s/%s [%d]\n",
                 a->major, a->minor, a->major_int, tmp, minor, major));
      if (a->major_int == major && !mutt_strcasecmp(minor, a->minor))
      {
	dprint(5, (debugfile, "parse_unattach_list: removed %s/%s [%d]\n",
                   a->major, a->minor, a->major_int));
	regfree(&a->minor_rx);
	FREE(&a->major);

	/* Relink backward */
	if (lastp)
	  lastp->next = lp->next;
	else
	  *ldata = lp->next;

        newlp = lp->next;
        FREE(&lp->data);	/* same as a */
        FREE(&lp);
        lp = newlp;
        continue;
      }

      lastp = lp;
      lp = lp->next;
    }

  }
  while (MoreArgs (s));

  FREE(&tmp);
  _attachments_clean();
  return 0;
}

static int print_attach_list (LIST *lp, char op, char *name)
{
  while (lp)
  {
    printf("attachments %c%s %s/%s\n", op, name,
           ((ATTACH_MATCH *)lp->data)->major,
           ((ATTACH_MATCH *)lp->data)->minor);
    lp = lp->next;
  }

  return 0;
}


static int parse_attachments (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  char op, *category;
  LIST **listp;

  mutt_extract_token(buf, s, 0);
  if (!buf->data || *buf->data == '\0')
  {
    strfcpy(err->data, _("attachments: no disposition"), err->dsize);
    return -1;
  }

  category = buf->data;
  op = *category++;

  if (op == '?')
  {
    mutt_endwin (NULL);
    fflush (stdout);
    printf("\nCurrent attachments settings:\n\n");
    print_attach_list(AttachAllow,   '+', "A");
    print_attach_list(AttachExclude, '-', "A");
    print_attach_list(InlineAllow,   '+', "I");
    print_attach_list(InlineExclude, '-', "I");
    print_attach_list(RootAllow,     '+', "R");
    print_attach_list(RootExclude,   '-', "R");
    mutt_any_key_to_continue (NULL);
    return 0;
  }

  if (op != '+' && op != '-')
  {
    op = '+';
    category--;
  }
  if (!ascii_strncasecmp(category, "attachment", strlen(category)))
  {
    if (op == '+')
      listp = &AttachAllow;
    else
      listp = &AttachExclude;
  }
  else if (!ascii_strncasecmp(category, "inline", strlen(category)))
  {
    if (op == '+')
      listp = &InlineAllow;
    else
      listp = &InlineExclude;
  }
  else if (!ascii_strncasecmp(category, "root", strlen(category)))
  {
    if (op == '+')
      listp = &RootAllow;
    else
      listp = &RootExclude;
  }
  else
  {
    strfcpy(err->data, _("attachments: invalid disposition"), err->dsize);
    return -1;
  }

  return parse_attach_list(buf, s, listp, err);
}

/*
 * Cleanup a single LIST item who's data is an ATTACH_MATCH
 */
static void free_attachments_data (char **data)
{
   if (data == NULL ||  *data == NULL) return;
   ATTACH_MATCH *a = (ATTACH_MATCH *) *data;
   regfree(&a->minor_rx);
   FREE (&a->major);
   FREE (data); /*__FREE_CHECKED__*/
}

static int parse_unattachments (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  char op, *p;
  LIST **listp;

  mutt_extract_token(buf, s, 0);
  if (!buf->data || *buf->data == '\0')
  {
    strfcpy(err->data, _("unattachments: no disposition"), err->dsize);
    return -1;
  }

  p = buf->data;
  op = *p++;

  if (op == '*')
  {
      mutt_free_list_generic(&AttachAllow, free_attachments_data);
      mutt_free_list_generic(&AttachExclude, free_attachments_data);
      mutt_free_list_generic(&InlineAllow, free_attachments_data);
      mutt_free_list_generic(&InlineExclude, free_attachments_data);
      mutt_free_list_generic(&RootAllow, free_attachments_data);
      mutt_free_list_generic(&RootExclude, free_attachments_data);
      _attachments_clean();
      return 0;
  }

  if (op != '+' && op != '-')
  {
    op = '+';
    p--;
  }
  if (!ascii_strncasecmp(p, "attachment", strlen(p)))
  {
    if (op == '+')
      listp = &AttachAllow;
    else
      listp = &AttachExclude;
  }
  else if (!ascii_strncasecmp(p, "inline", strlen(p)))
  {
    if (op == '+')
      listp = &InlineAllow;
    else
      listp = &InlineExclude;
  }
  else if (!ascii_strncasecmp(p, "root", strlen(p)))
  {
    if (op == '+')
      listp = &RootAllow;
    else
      listp = &RootExclude;
  }
  else
  {
    strfcpy(err->data, _("unattachments: invalid disposition"), err->dsize);
    return -1;
  }

  return parse_unattach_list(buf, s, listp, err);
}

static int parse_unlists (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  hash_destroy (&AutoSubscribeCache, NULL);
  do
  {
    mutt_extract_token (buf, s, 0);
    mutt_remove_from_rx_list (&SubscribedLists, buf->data);
    mutt_remove_from_rx_list (&MailLists, buf->data);

    if (mutt_strcmp (buf->data, "*") &&
	mutt_add_to_rx_list (&UnMailLists, buf->data, REG_ICASE, err) != 0)
      return -1;
  }
  while (MoreArgs (s));

  return 0;
}

static int parse_subscribe (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  group_context_t *gc = NULL;

  do
  {
    mutt_extract_token (buf, s, 0);

    if (parse_group_context (&gc, buf, s, err) == -1)
      goto bail;

    mutt_remove_from_rx_list (&UnMailLists, buf->data);
    mutt_remove_from_rx_list (&UnSubscribedLists, buf->data);

    if (mutt_add_to_rx_list (&MailLists, buf->data, REG_ICASE, err) != 0)
      goto bail;
    if (mutt_add_to_rx_list (&SubscribedLists, buf->data, REG_ICASE, err) != 0)
      goto bail;
    if (mutt_group_context_add_rx (gc, buf->data, REG_ICASE, err) != 0)
      goto bail;
  }
  while (MoreArgs (s));

  mutt_group_context_destroy (&gc);
  return 0;

bail:
  mutt_group_context_destroy (&gc);
  return -1;
}

static int parse_unsubscribe (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  hash_destroy (&AutoSubscribeCache, NULL);
  do
  {
    mutt_extract_token (buf, s, 0);
    mutt_remove_from_rx_list (&SubscribedLists, buf->data);

    if (mutt_strcmp (buf->data, "*") &&
	mutt_add_to_rx_list (&UnSubscribedLists, buf->data, REG_ICASE, err) != 0)
      return -1;
  }
  while (MoreArgs (s));

  return 0;
}

static int parse_unalias (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  ALIAS *tmp, *last = NULL;

  do
  {
    mutt_extract_token (buf, s, 0);

    if (mutt_strcmp ("*", buf->data) == 0)
    {
      if (CurrentMenu == MENU_ALIAS)
      {
	for (tmp = Aliases; tmp ; tmp = tmp->next)
	  tmp->del = 1;
	mutt_set_current_menu_redraw_full ();
      }
      else
	mutt_free_alias (&Aliases);
      break;
    }
    else
      for (tmp = Aliases; tmp; tmp = tmp->next)
      {
	if (mutt_strcasecmp (buf->data, tmp->name) == 0)
	{
	  if (CurrentMenu == MENU_ALIAS)
	  {
	    tmp->del = 1;
	    mutt_set_current_menu_redraw_full ();
	    break;
	  }

	  if (last)
	    last->next = tmp->next;
	  else
	    Aliases = tmp->next;
	  tmp->next = NULL;
	  mutt_free_alias (&tmp);
	  break;
	}
	last = tmp;
      }
  }
  while (MoreArgs (s));
  return 0;
}

static int parse_alias (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  ALIAS *tmp = Aliases;
  ALIAS *last = NULL;
  char *estr = NULL;
  group_context_t *gc = NULL;

  if (!MoreArgs (s))
  {
    strfcpy (err->data, _("alias: no address"), err->dsize);
    return (-1);
  }

  mutt_extract_token (buf, s, 0);

  if (parse_group_context (&gc, buf, s, err) == -1)
    return -1;

  /* check to see if an alias with this name already exists */
  for (; tmp; tmp = tmp->next)
  {
    if (!mutt_strcasecmp (tmp->name, buf->data))
      break;
    last = tmp;
  }

  if (!tmp)
  {
    /* create a new alias */
    tmp = (ALIAS *) safe_calloc (1, sizeof (ALIAS));
    tmp->self = tmp;
    tmp->name = safe_strdup (buf->data);
    /* give the main addressbook code a chance */
    if (CurrentMenu == MENU_ALIAS)
      set_option (OPTMENUCALLER);
  }
  else
  {
    mutt_alias_delete_reverse (tmp);
    /* override the previous value */
    rfc822_free_address (&tmp->addr);
    if (CurrentMenu == MENU_ALIAS)
      mutt_set_current_menu_redraw_full ();
  }

  mutt_extract_token (buf, s, MUTT_TOKEN_QUOTE | MUTT_TOKEN_SPACE | MUTT_TOKEN_SEMICOLON);
  dprint (3, (debugfile, "parse_alias: Second token is '%s'.\n",
	      buf->data));

  tmp->addr = mutt_parse_adrlist (tmp->addr, buf->data);

  if (last)
    last->next = tmp;
  else
    Aliases = tmp;
  if (mutt_addrlist_to_intl (tmp->addr, &estr))
  {
    snprintf (err->data, err->dsize, _("Warning: Bad IDN '%s' in alias '%s'.\n"),
	      estr, tmp->name);
    FREE (&estr);
    goto bail;
  }

  mutt_group_context_add_adrlist (gc, tmp->addr);
  mutt_alias_add_reverse (tmp);

#ifdef DEBUG
  if (debuglevel >= 2)
  {
    ADDRESS *a;
    /* A group is terminated with an empty address, so check a->mailbox */
    for (a = tmp->addr; a && a->mailbox; a = a->next)
    {
      if (!a->group)
	dprint (3, (debugfile, "parse_alias:   %s\n",
		    a->mailbox));
      else
	dprint (3, (debugfile, "parse_alias:   Group %s\n",
		    a->mailbox));
    }
  }
#endif
  mutt_group_context_destroy (&gc);
  return 0;

bail:
  mutt_group_context_destroy (&gc);
  return -1;
}

static int
parse_unmy_hdr (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  LIST *last = NULL;
  LIST *tmp = UserHeader;
  LIST *ptr;
  size_t l;

  do
  {
    mutt_extract_token (buf, s, 0);
    if (mutt_strcmp ("*", buf->data) == 0)
      mutt_free_list (&UserHeader);
    else
    {
      tmp = UserHeader;
      last = NULL;

      l = mutt_strlen (buf->data);
      if (buf->data[l - 1] == ':')
	l--;

      while (tmp)
      {
	if (ascii_strncasecmp (buf->data, tmp->data, l) == 0 && tmp->data[l] == ':')
	{
	  ptr = tmp;
	  if (last)
	    last->next = tmp->next;
	  else
	    UserHeader = tmp->next;
	  tmp = tmp->next;
	  ptr->next = NULL;
	  mutt_free_list (&ptr);
	}
	else
	{
	  last = tmp;
	  tmp = tmp->next;
	}
      }
    }
  }
  while (MoreArgs (s));
  return 0;
}

static int update_my_hdr (const char *my_hdr)
{
  LIST *tmp;
  size_t keylen;
  const char *p;

  if (!my_hdr)
    return -1;

  if ((p = strpbrk (my_hdr, ": \t")) == NULL || *p != ':')
    return -1;
  keylen = p - my_hdr + 1;

  if (UserHeader)
  {
    for (tmp = UserHeader; ; tmp = tmp->next)
    {
      /* see if there is already a field by this name */
      if (ascii_strncasecmp (my_hdr, tmp->data, keylen) == 0)
      {
	/* replace the old value */
	mutt_str_replace (&tmp->data, my_hdr);
	return 0;
      }
      if (!tmp->next)
	break;
    }
    tmp->next = mutt_new_list ();
    tmp = tmp->next;
  }
  else
  {
    tmp = mutt_new_list ();
    UserHeader = tmp;
  }
  tmp->data = safe_strdup (my_hdr);

  return 0;
}

static int parse_my_hdr (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  mutt_extract_token (buf, s, MUTT_TOKEN_SPACE | MUTT_TOKEN_QUOTE);
  if (update_my_hdr (mutt_b2s (buf)))
  {
    strfcpy (err->data, _("invalid header field"), err->dsize);
    return -1;
  }

  return 0;
}

static int
parse_sort (short *val, const char *s, const struct mapping_t *map, BUFFER *err)
{
  int i, flags = 0;

  if (mutt_strncmp ("reverse-", s, 8) == 0)
  {
    s += 8;
    flags = SORT_REVERSE;
  }

  if (mutt_strncmp ("last-", s, 5) == 0)
  {
    s += 5;
    flags |= SORT_LAST;
  }

  if ((i = mutt_getvaluebyname (s, map)) == -1)
  {
    snprintf (err->data, err->dsize, _("%s: unknown sorting method"), s);
    return (-1);
  }

  *val = i | flags;

  return 0;
}

static void mutt_set_default (struct option_t *p)
{
  switch (p->type & DT_MASK)
  {
    case DT_STR:
    case DT_PATH:
    case DT_CMD_PATH:
      if (!p->init.p && *((char **) p->data.p))
        p->init.p = safe_strdup (* ((char **) p->data.p));
      else if (p->init.p && (p->type & DT_L10N_STR))
        p->init.p = safe_strdup (_(p->init.p));
      break;
    case DT_ADDR:
      if (!p->init.p && *((ADDRESS **) p->data.p))
      {
	char tmp[HUGE_STRING];
	*tmp = '\0';
	rfc822_write_address (tmp, sizeof (tmp), *((ADDRESS **) p->data.p), 0);
	p->init.p = safe_strdup (tmp);
      }
      break;
    case DT_RX:
    {
      REGEXP *pp = (REGEXP *) p->data.p;
      if (!p->init.p && pp->pattern)
	p->init.p = safe_strdup (pp->pattern);
      else if (p->init.p && (p->type & DT_L10N_STR))
        p->init.p = safe_strdup (_(p->init.p));
      break;
    }
  }
}

static void mutt_restore_default (struct option_t *p)
{
  switch (p->type & DT_MASK)
  {
    case DT_STR:
      mutt_str_replace ((char **) p->data.p, (char *) p->init.p);
      break;
    case DT_MBCHARTBL:
      free_mbchar_table ((mbchar_table **)p->data.p);
      *((mbchar_table **) p->data.p) = parse_mbchar_table ((char *) p->init.p);
      break;
    case DT_PATH:
    case DT_CMD_PATH:
      FREE((char **) p->data.p);		/* __FREE_CHECKED__ */
      if (p->init.p)
      {
	BUFFER *path;
        path = mutt_buffer_pool_get ();
	mutt_buffer_strcpy (path, (char *) p->init.p);
        if (DTYPE (p->type) == DT_CMD_PATH)
          mutt_buffer_expand_path_norel (path);
        else
          mutt_buffer_expand_path (path);
	*((char **) p->data.p) = safe_strdup (mutt_b2s (path));
        mutt_buffer_pool_release (&path);
      }
      break;
    case DT_ADDR:
      rfc822_free_address ((ADDRESS **) p->data.p);
      if (p->init.p)
	*((ADDRESS **) p->data.p) = rfc822_parse_adrlist (NULL, (char *) p->init.p);
      break;
    case DT_BOOL:
      if (p->init.l)
	set_option (p->data.l);
      else
	unset_option (p->data.l);
      break;
    case DT_QUAD:
      set_quadoption (p->data.l, p->init.l);
      break;
    case DT_NUM:
    case DT_SORT:
    case DT_MAGIC:
      *((short *) p->data.p) = p->init.l;
      break;
    case DT_LNUM:
      *((long *) p->data.p) = p->init.l;
      break;
    case DT_RX:
    {
      REGEXP *pp = (REGEXP *) p->data.p;
      int flags = 0;

      FREE (&pp->pattern);
      if (pp->rx)
      {
        regfree (pp->rx);
        FREE (&pp->rx);
      }

      if (p->init.p)
      {
        char *s = (char *) p->init.p;

        pp->rx = safe_calloc (1, sizeof (regex_t));
        pp->pattern = safe_strdup ((char *) p->init.p);
        if (mutt_strcmp (p->option, "mask") != 0)
          flags |= mutt_which_case ((const char *) p->init.p);
        if (mutt_strcmp (p->option, "mask") == 0 && *s == '!')
        {
          s++;
          pp->not = 1;
        }
        if (REGCOMP (pp->rx, s, flags) != 0)
        {
          fprintf (stderr, _("mutt_restore_default(%s): error in regexp: %s\n"),
                   p->option, pp->pattern);
          FREE (&pp->pattern);
          FREE (&pp->rx);
        }
      }
    }
    break;
  }

  if (p->flags & R_INDEX)
    mutt_set_menu_redraw_full (MENU_MAIN);
  if (p->flags & R_PAGER)
    mutt_set_menu_redraw_full (MENU_PAGER);
  if (p->flags & R_PAGER_FLOW)
  {
    mutt_set_menu_redraw_full (MENU_PAGER);
    mutt_set_menu_redraw (MENU_PAGER, REDRAW_FLOW);
  }
  if (p->flags & R_RESORT_SUB)
    set_option (OPTSORTSUBTHREADS);
  if (p->flags & R_RESORT)
    set_option (OPTNEEDRESORT);
  if (p->flags & R_RESORT_INIT)
    set_option (OPTRESORTINIT);
  if (p->flags & R_TREE)
    set_option (OPTREDRAWTREE);
  if (p->flags & R_REFLOW)
    mutt_reflow_windows ();
#ifdef USE_SIDEBAR
  if (p->flags & R_SIDEBAR)
    mutt_set_current_menu_redraw (REDRAW_SIDEBAR);
#endif
  if (p->flags & R_MENU)
    mutt_set_current_menu_redraw_full ();
}

static void escape_string_to_buffer (BUFFER *dst, const char *src)
{
  mutt_buffer_clear (dst);

  if (!src || !*src)
    return;

  for (; *src; src++)
  {
    switch (*src)
    {
      case '\n':
        mutt_buffer_addstr (dst, "\\n");
        break;
      case '\r':
        mutt_buffer_addstr (dst, "\\r");
        break;
      case '\t':
        mutt_buffer_addstr (dst, "\\t");
        break;
      case '\\':
      case '"':
        mutt_buffer_addch (dst, '\\');
        /* fall through */
      default:
        mutt_buffer_addch (dst, *src);
    }
  }
}

static size_t escape_string (char *dst, size_t len, const char* src)
{
  char* p = dst;

  if (!len)
    return 0;
  len--; /* save room for \0 */
#define ESC_CHAR(C)	do { *p++ = '\\'; if (p - dst < len) *p++ = C; } while (0)
  while (p - dst < len && src && *src)
  {
    switch (*src)
    {
      case '\n':
        ESC_CHAR('n');
        break;
      case '\r':
        ESC_CHAR('r');
        break;
      case '\t':
        ESC_CHAR('t');
        break;
      default:
        if ((*src == '\\' || *src == '"') && p - dst < len - 1)
          *p++ = '\\';
        *p++ = *src;
    }
    src++;
  }
#undef ESC_CHAR
  *p = '\0';
  return p - dst;
}

static void pretty_var (char *dst, size_t len, const char *option, const char *val)
{
  char *p;

  if (!len)
    return;

  strfcpy (dst, option, len);
  len--; /* save room for \0 */
  p = dst + mutt_strlen (dst);

  if (p - dst < len)
    *p++ = '=';
  if (p - dst < len)
    *p++ = '"';
  p += escape_string (p, len - (p - dst) + 1, val);	/* \0 terminate it */
  if (p - dst < len)
    *p++ = '"';
  *p = 0;
}

static int check_charset (struct option_t *opt, const char *val)
{
  char *p, *q = NULL, *s = safe_strdup (val);
  int rc = 0, strict = strcmp (opt->option, "send_charset") == 0;

  /* $charset should be nonempty, and a single value - not a colon
   * delimited list */
  if (mutt_strcmp (opt->option, "charset") == 0)
  {
    if (!s || (strchr (s, ':') != NULL))
    {
      FREE (&s);
      return -1;
    }
  }

  /* other values can be empty */
  if (!s)
    return rc;

  for (p = strtok_r (s, ":", &q); p; p = strtok_r (NULL, ":", &q))
  {
    if (!*p)
      continue;
    if (mutt_check_charset (p, strict) < 0)
    {
      rc = -1;
      break;
    }
  }

  FREE(&s);
  return rc;
}

char **mutt_envlist (void)
{
  return envlist;
}

/* Helper function for parse_setenv().
 * It's broken out because some other parts of mutt (filter.c) need
 * to set/overwrite environment variables in envlist before execing.
 */
void mutt_envlist_set (const char *name, const char *value, int overwrite)
{
  char **envp = envlist;
  char work[LONG_STRING];
  int count;
  size_t len;

  len = mutt_strlen (name);

  /* Look for current slot to overwrite */
  count = 0;
  while (envp && *envp)
  {
    if (!mutt_strncmp (name, *envp, len) && (*envp)[len] == '=')
    {
      if (!overwrite)
        return;
      break;
    }
    envp++;
    count++;
  }

  /* Format var=value string */
  snprintf (work, sizeof(work), "%s=%s", NONULL (name), NONULL (value));

  /* If slot found, overwrite */
  if (envp && *envp)
    mutt_str_replace (envp, work);

  /* If not found, add new slot */
  else
  {
    safe_realloc (&envlist, sizeof(char *) * (count + 2));
    envlist[count] = safe_strdup (work);
    envlist[count + 1] = NULL;
  }
}

static int parse_setenv(BUFFER *tmp, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int query, unset;
  size_t len;
  char *name, **save, **envp = envlist;
  int count = 0;
  long data = udata.l;

  query = 0;
  unset = data & MUTT_SET_UNSET;

  if (!MoreArgs (s))
  {
    strfcpy (err->data, _("too few arguments"), err->dsize);
    return -1;
  }

  if (*s->dptr == '?')
  {
    query = 1;
    s->dptr++;
  }

  /* get variable name */
  mutt_extract_token (tmp, s, MUTT_TOKEN_EQUAL);
  len = strlen (tmp->data);

  if (query)
  {
    int found = 0;
    while (envp && *envp)
    {
      if (!mutt_strncmp (tmp->data, *envp, len))
      {
        if (!found)
        {
          mutt_endwin (NULL);
          found = 1;
        }
        puts (*envp);
      }
      envp++;
    }

    if (found)
    {
      mutt_any_key_to_continue (NULL);
      return 0;
    }

    snprintf (err->data, err->dsize, _("%s is unset"), tmp->data);
    return 0;
  }

  if (unset)
  {
    count = 0;
    while (envp && *envp)
    {
      if (!mutt_strncmp (tmp->data, *envp, len) && (*envp)[len] == '=')
      {
        /* shuffle down */
        save = envp++;
        while (*envp)
        {
          *save++ = *envp++;
          count++;
        }
        *save = NULL;
        safe_realloc (&envlist, sizeof(char *) * (count+1));
        return 0;
      }
      envp++;
      count++;
    }

    snprintf (err->data, err->dsize, _("%s is unset"), tmp->data);
    return 0;
  }

  /* set variable */

  if (*s->dptr == '=')
  {
    s->dptr++;
    SKIPWS (s->dptr);
  }

  if (!MoreArgs (s))
  {
    strfcpy (err->data, _("too few arguments"), err->dsize);
    return -1;
  }

  name = safe_strdup (tmp->data);
  mutt_extract_token (tmp, s, 0);
  mutt_envlist_set (name, tmp->data, 1);
  FREE (&name);

  return 0;
}

static int parse_set (BUFFER *tmp, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int query, unset, inv, reset, r = 0;
  int idx = -1;
  const char *p;
  BUFFER *scratch = NULL;
  char* myvar;
  long data = udata.l;

  while (MoreArgs (s))
  {
    /* reset state variables */
    query = 0;
    unset = data & MUTT_SET_UNSET;
    inv = data & MUTT_SET_INV;
    reset = data & MUTT_SET_RESET;
    myvar = NULL;

    if (*s->dptr == '?')
    {
      query = 1;
      s->dptr++;
    }
    else if (mutt_strncmp ("no", s->dptr, 2) == 0)
    {
      s->dptr += 2;
      unset = !unset;
    }
    else if (mutt_strncmp ("inv", s->dptr, 3) == 0)
    {
      s->dptr += 3;
      inv = !inv;
    }
    else if (*s->dptr == '&')
    {
      reset = 1;
      s->dptr++;
    }

    /* get the variable name */
    mutt_extract_token (tmp, s, MUTT_TOKEN_EQUAL);

    if (!mutt_strncmp ("my_", tmp->data, 3))
      myvar = tmp->data;
    else if ((idx = mutt_option_index (tmp->data)) == -1 &&
             !(reset && !mutt_strcmp ("all", tmp->data)))
    {
      snprintf (err->data, err->dsize, _("%s: unknown variable"), tmp->data);
      return (-1);
    }
    SKIPWS (s->dptr);

    if (reset)
    {
      if (query || unset || inv)
      {
	snprintf (err->data, err->dsize, "%s", _("prefix is illegal with reset"));
	return (-1);
      }

      if (s && *s->dptr == '=')
      {
	snprintf (err->data, err->dsize, "%s", _("value is illegal with reset"));
	return (-1);
      }

      if (!mutt_strcmp ("all", tmp->data))
      {
	if (CurrentMenu == MENU_PAGER)
	{
	  snprintf (err->data, err->dsize, "%s", _("Not available in this menu."));
	  return (-1);
	}
	for (idx = 0; MuttVars[idx].option; idx++)
	  mutt_restore_default (&MuttVars[idx]);
	mutt_set_current_menu_redraw_full ();
	set_option (OPTSORTSUBTHREADS);
	set_option (OPTNEEDRESORT);
	set_option (OPTRESORTINIT);
	set_option (OPTREDRAWTREE);
	return 0;
      }
      else
      {
	CHECK_PAGER;
        if (myvar)
          myvar_del (myvar);
        else
          mutt_restore_default (&MuttVars[idx]);
      }
    }
    else if (!myvar && DTYPE (MuttVars[idx].type) == DT_BOOL)
    {
      if (s && *s->dptr == '=')
      {
	if (unset || inv || query)
	{
	  snprintf (err->data, err->dsize, "%s", _("Usage: set variable=yes|no"));
	  return (-1);
	}

	s->dptr++;
	mutt_extract_token (tmp, s, 0);
	if (ascii_strcasecmp ("yes", tmp->data) == 0)
	  unset = inv = 0;
	else if (ascii_strcasecmp ("no", tmp->data) == 0)
	  unset = 1;
	else
	{
	  snprintf (err->data, err->dsize, "%s", _("Usage: set variable=yes|no"));
	  return (-1);
	}
      }

      if (query)
      {
	snprintf (err->data, err->dsize, option (MuttVars[idx].data.l)
                  ? _("%s is set") : _("%s is unset"), tmp->data);
	return 0;
      }

      CHECK_PAGER;
      if (unset)
	unset_option (MuttVars[idx].data.l);
      else if (inv)
	toggle_option (MuttVars[idx].data.l);
      else
	set_option (MuttVars[idx].data.l);
    }
    else if (myvar || DTYPE (MuttVars[idx].type) == DT_STR ||
	     DTYPE (MuttVars[idx].type) == DT_PATH ||
	     DTYPE (MuttVars[idx].type) == DT_CMD_PATH ||
	     DTYPE (MuttVars[idx].type) == DT_ADDR ||
	     DTYPE (MuttVars[idx].type) == DT_MBCHARTBL)
    {
      if (unset)
      {
	CHECK_PAGER;
        if (myvar)
          myvar_del (myvar);
	else if (DTYPE (MuttVars[idx].type) == DT_ADDR)
	  rfc822_free_address ((ADDRESS **) MuttVars[idx].data.p);
	else if (DTYPE (MuttVars[idx].type) == DT_MBCHARTBL)
          free_mbchar_table ((mbchar_table **) MuttVars[idx].data.p);
	else
	  /* MuttVars[idx].data.p is already 'char**' (or some 'void**') or...
	   * so cast to 'void*' is okay */
	  FREE (MuttVars[idx].data.p);		/* __FREE_CHECKED__ */
      }
      else if (query || *s->dptr != '=')
      {
	char _tmp[LONG_STRING];
	const char *val = NULL;
        BUFFER *path_buf = NULL;

        if (myvar)
        {
          if ((val = myvar_get (myvar)))
          {
	    pretty_var (err->data, err->dsize, myvar, val);
            break;
          }
          else
          {
            snprintf (err->data, err->dsize, _("%s: unknown variable"), myvar);
            return (-1);
          }
        }
	else if (DTYPE (MuttVars[idx].type) == DT_ADDR)
	{
	  _tmp[0] = '\0';
	  rfc822_write_address (_tmp, sizeof (_tmp), *((ADDRESS **) MuttVars[idx].data.p), 0);
	  val = _tmp;
	}
	else if (DTYPE (MuttVars[idx].type) == DT_PATH ||
                 DTYPE (MuttVars[idx].type) == DT_CMD_PATH)
	{
          path_buf = mutt_buffer_pool_get ();
          mutt_buffer_strcpy (path_buf, NONULL(*((char **) MuttVars[idx].data.p)));
          if (mutt_strcmp (MuttVars[idx].option, "record") == 0)
            mutt_buffer_pretty_multi_mailbox (path_buf, FccDelimiter);
          else
            mutt_buffer_pretty_mailbox (path_buf);
	  val = mutt_b2s (path_buf);
	}
	else if (DTYPE (MuttVars[idx].type) == DT_MBCHARTBL)
        {
          mbchar_table *mbt = (*((mbchar_table **) MuttVars[idx].data.p));
          val = mbt ? NONULL (mbt->orig_str) : "";
        }
	else
	  val = *((char **) MuttVars[idx].data.p);

	/* user requested the value of this variable */
	pretty_var (err->data, err->dsize, MuttVars[idx].option, NONULL(val));

        mutt_buffer_pool_release (&path_buf);
	break;
      }
      else
      {
	CHECK_PAGER;
        s->dptr++;

        if (myvar)
	{
	  /* myvar is a pointer to tmp and will be lost on extract_token */
	  myvar = safe_strdup (myvar);
	}

        mutt_extract_token (tmp, s, 0);

        if (myvar)
        {
          myvar_set (myvar, tmp->data);
          FREE (&myvar);
	  myvar="don't resort";
        }
        else if (DTYPE (MuttVars[idx].type) == DT_PATH ||
                 DTYPE (MuttVars[idx].type) == DT_CMD_PATH)
        {
	  /* MuttVars[idx].data is already 'char**' (or some 'void**') or...
	   * so cast to 'void*' is okay */
	  FREE (MuttVars[idx].data.p);		/* __FREE_CHECKED__ */

          scratch = mutt_buffer_pool_get ();
	  mutt_buffer_strcpy (scratch, tmp->data);
          if (mutt_strcmp (MuttVars[idx].option, "record") == 0)
            mutt_buffer_expand_multi_path (scratch, FccDelimiter);
          else if ((mutt_strcmp (MuttVars[idx].option, "signature") == 0) &&
                   mutt_buffer_len (scratch) &&
                   (*(scratch->dptr - 1) == '|'))
            mutt_buffer_expand_path_norel (scratch);
          else if (DTYPE (MuttVars[idx].type) == DT_CMD_PATH)
            mutt_buffer_expand_path_norel (scratch);
          else
            mutt_buffer_expand_path (scratch);
	  *((char **) MuttVars[idx].data.p) = safe_strdup (mutt_b2s (scratch));
          mutt_buffer_pool_release (&scratch);
        }
        else if (DTYPE (MuttVars[idx].type) == DT_STR)
        {
	  if (strstr (MuttVars[idx].option, "charset") &&
              check_charset (&MuttVars[idx], tmp->data) < 0)
	  {
	    snprintf (err->data, err->dsize, _("Invalid value for option %s: \"%s\""),
		      MuttVars[idx].option, tmp->data);
	    return (-1);
	  }

	  FREE (MuttVars[idx].data.p);		/* __FREE_CHECKED__ */
	  *((char **) MuttVars[idx].data.p) = safe_strdup (tmp->data);
	  if (mutt_strcmp (MuttVars[idx].option, "charset") == 0)
	    mutt_set_charset (Charset);
        }
        else if (DTYPE (MuttVars[idx].type) == DT_MBCHARTBL)
        {
          free_mbchar_table ((mbchar_table **) MuttVars[idx].data.p);
          *((mbchar_table **) MuttVars[idx].data.p) = parse_mbchar_table (tmp->data);
        }
        else
        {
	  rfc822_free_address ((ADDRESS **) MuttVars[idx].data.p);
	  *((ADDRESS **) MuttVars[idx].data.p) = rfc822_parse_adrlist (NULL, tmp->data);
        }
      }
    }
    else if (DTYPE(MuttVars[idx].type) == DT_RX)
    {
      REGEXP *ptr = (REGEXP *) MuttVars[idx].data.p;
      regex_t *rx;
      int e, flags = 0;

      if (query || *s->dptr != '=')
      {
	/* user requested the value of this variable */
	pretty_var (err->data, err->dsize, MuttVars[idx].option, NONULL(ptr->pattern));
	break;
      }

      if (option(OPTATTACHMSG) && !mutt_strcmp(MuttVars[idx].option, "reply_regexp"))
      {
	snprintf (err->data, err->dsize, "Operation not permitted when in attach-message mode.");
	r = -1;
	break;
      }

      CHECK_PAGER;
      s->dptr++;

      /* copy the value of the string */
      mutt_extract_token (tmp, s, 0);

      if (!ptr->pattern || mutt_strcmp (ptr->pattern, tmp->data) != 0)
      {
	int not = 0;

	/* $mask is case-sensitive */
	if (mutt_strcmp (MuttVars[idx].option, "mask") != 0)
	  flags |= mutt_which_case (tmp->data);

	p = tmp->data;
	if (mutt_strcmp (MuttVars[idx].option, "mask") == 0)
	{
	  if (*p == '!')
	  {
	    not = 1;
	    p++;
	  }
	}

	rx = (regex_t *) safe_malloc (sizeof (regex_t));
	if ((e = REGCOMP (rx, p, flags)) != 0)
	{
	  regerror (e, rx, err->data, err->dsize);
	  FREE (&rx);
	  break;
	}

	/* get here only if everything went smootly */
	if (ptr->pattern)
	{
	  FREE (&ptr->pattern);
	  regfree ((regex_t *) ptr->rx);
	  FREE (&ptr->rx);
	}

	ptr->pattern = safe_strdup (tmp->data);
	ptr->rx = rx;
	ptr->not = not;

	/* $reply_regexp and $alterantes require special treatment */

	if (Context && Context->msgcount &&
	    mutt_strcmp (MuttVars[idx].option, "reply_regexp") == 0)
	{
	  regmatch_t pmatch[1];
	  int i;

          hash_destroy (&Context->subj_hash, NULL);
	  for (i = 0; i < Context->msgcount; i++)
	  {
	    ENVELOPE *cur_env = Context->hdrs[i]->env;

	    if (cur_env && cur_env->subject)
	    {
	      cur_env->real_subj =
                (regexec (ReplyRegexp.rx, cur_env->subject, 1, pmatch, 0)) ?
                cur_env->subject :
                cur_env->subject + pmatch[0].rm_eo;
	    }
	  }
	}
      }
    }
    else if (DTYPE(MuttVars[idx].type) == DT_MAGIC)
    {
      if (query || *s->dptr != '=')
      {
	switch (DefaultMagic)
	{
	  case MUTT_MBOX:
	    p = "mbox";
	    break;
	  case MUTT_MMDF:
	    p = "MMDF";
	    break;
	  case MUTT_MH:
	    p = "MH";
	    break;
	  case MUTT_MAILDIR:
	    p = "Maildir";
	    break;
	  default:
	    p = "unknown";
	    break;
	}
	snprintf (err->data, err->dsize, "%s=%s", MuttVars[idx].option, p);
	break;
      }

      CHECK_PAGER;
      s->dptr++;

      /* copy the value of the string */
      mutt_extract_token (tmp, s, 0);
      if (mx_set_magic (tmp->data))
      {
	snprintf (err->data, err->dsize, _("%s: invalid mailbox type"), tmp->data);
	r = -1;
	break;
      }
    }
    else if (DTYPE(MuttVars[idx].type) == DT_NUM)
    {
      short *ptr = (short *) MuttVars[idx].data.p;
      short val;
      int rc;

      if (query || *s->dptr != '=')
      {
	val = *ptr;
	/* compatibility alias */
	if (mutt_strcmp (MuttVars[idx].option, "wrapmargin") == 0)
	  val = *ptr < 0 ? -*ptr : 0;

	/* user requested the value of this variable */
	snprintf (err->data, err->dsize, "%s=%d", MuttVars[idx].option, val);
	break;
      }

      CHECK_PAGER;
      s->dptr++;

      mutt_extract_token (tmp, s, 0);
      rc = mutt_atos (tmp->data, (short *) &val, 0);
      if (rc < 0)
      {
	snprintf (err->data, err->dsize, _("%s: invalid value (%s)"), tmp->data,
		  rc == -1 ? _("format error") : _("number overflow"));
	r = -1;
	break;
      }
      else
	*ptr = val;

      /* these ones need a sanity check */
      if (mutt_strcmp (MuttVars[idx].option, "history") == 0)
      {
	if (*ptr < 0)
	  *ptr = 0;
	mutt_init_history ();
      }
      else if (mutt_strcmp (MuttVars[idx].option, "error_history") == 0)
      {
	if (*ptr < 0)
	  *ptr = 0;
	mutt_error_history_init ();
      }
      else if (mutt_strcmp (MuttVars[idx].option, "pager_index_lines") == 0)
      {
	if (*ptr < 0)
	  *ptr = 0;
      }
      else if (mutt_strcmp (MuttVars[idx].option, "wrapmargin") == 0)
      {
	if (*ptr < 0)
	  *ptr = 0;
	else
	  *ptr = -*ptr;
      }
#ifdef USE_IMAP
      else if (mutt_strcmp (MuttVars[idx].option, "imap_pipeline_depth") == 0)
      {
        if (*ptr < 0)
          *ptr = 0;
      }
#endif
    }
    else if (DTYPE(MuttVars[idx].type) == DT_LNUM)
    {
      long *ptr = (long *) MuttVars[idx].data.p;
      long val;
      int rc;

      if (query || *s->dptr != '=')
      {
	val = *ptr;

	/* user requested the value of this variable */
	snprintf (err->data, err->dsize, "%s=%ld", MuttVars[idx].option, val);
	break;
      }

      CHECK_PAGER;
      s->dptr++;

      mutt_extract_token (tmp, s, 0);
      rc = mutt_atol (tmp->data, (long *) &val, 0);
      if (rc < 0)
      {
	snprintf (err->data, err->dsize, _("%s: invalid value (%s)"), tmp->data,
		  rc == -1 ? _("format error") : _("number overflow"));
	r = -1;
	break;
      }
      else
	*ptr = val;

    }
    else if (DTYPE (MuttVars[idx].type) == DT_QUAD)
    {
      if (query)
      {
	static const char * const vals[] = { "no", "yes", "ask-no", "ask-yes" };

	snprintf (err->data, err->dsize, "%s=%s", MuttVars[idx].option,
		  vals [ quadoption (MuttVars[idx].data.l) ]);
	break;
      }

      CHECK_PAGER;
      if (*s->dptr == '=')
      {
	s->dptr++;
	mutt_extract_token (tmp, s, 0);
	if (ascii_strcasecmp ("yes", tmp->data) == 0)
	  set_quadoption (MuttVars[idx].data.l, MUTT_YES);
	else if (ascii_strcasecmp ("no", tmp->data) == 0)
	  set_quadoption (MuttVars[idx].data.l, MUTT_NO);
	else if (ascii_strcasecmp ("ask-yes", tmp->data) == 0)
	  set_quadoption (MuttVars[idx].data.l, MUTT_ASKYES);
	else if (ascii_strcasecmp ("ask-no", tmp->data) == 0)
	  set_quadoption (MuttVars[idx].data.l, MUTT_ASKNO);
	else
	{
	  snprintf (err->data, err->dsize, _("%s: invalid value"), tmp->data);
	  r = -1;
	  break;
	}
      }
      else
      {
	if (inv)
	  toggle_quadoption (MuttVars[idx].data.l);
	else if (unset)
	  set_quadoption (MuttVars[idx].data.l, MUTT_NO);
	else
	  set_quadoption (MuttVars[idx].data.l, MUTT_YES);
      }
    }
    else if (DTYPE (MuttVars[idx].type) == DT_SORT)
    {
      const struct mapping_t *map = NULL;

      switch (MuttVars[idx].type & DT_SUBTYPE_MASK)
      {
	case DT_SORT_ALIAS:
	  map = SortAliasMethods;
	  break;
	case DT_SORT_BROWSER:
	  map = SortBrowserMethods;
	  break;
	case DT_SORT_KEYS:
          if ((WithCrypto & APPLICATION_PGP))
            map = SortKeyMethods;
	  break;
	case DT_SORT_AUX:
	  map = SortAuxMethods;
	  break;
	case DT_SORT_SIDEBAR:
	  map = SortSidebarMethods;
	  break;
	case DT_SORT_THREAD_GROUPS:
	  map = SortThreadGroupsMethods;
	  break;
	default:
	  map = SortMethods;
	  break;
      }

      if (!map)
      {
	snprintf (err->data, err->dsize, _("%s: Unknown type."), MuttVars[idx].option);
	r = -1;
	break;
      }

      if (query || *s->dptr != '=')
      {
	p = mutt_getnamebyvalue (*((short *) MuttVars[idx].data.p) & SORT_MASK, map);

	snprintf (err->data, err->dsize, "%s=%s%s%s", MuttVars[idx].option,
		  (*((short *) MuttVars[idx].data.p) & SORT_REVERSE) ? "reverse-" : "",
		  (*((short *) MuttVars[idx].data.p) & SORT_LAST) ? "last-" : "",
		  p);
	return 0;
      }
      CHECK_PAGER;
      s->dptr++;
      mutt_extract_token (tmp, s , 0);

      if (parse_sort ((short *) MuttVars[idx].data.p, tmp->data, map, err) == -1)
      {
	r = -1;
	break;
      }
    }
    else
    {
      snprintf (err->data, err->dsize, _("%s: unknown type"), MuttVars[idx].option);
      r = -1;
      break;
    }

    if (!myvar)
    {
      if (MuttVars[idx].flags & R_INDEX)
        mutt_set_menu_redraw_full (MENU_MAIN);
      if (MuttVars[idx].flags & R_PAGER)
        mutt_set_menu_redraw_full (MENU_PAGER);
      if (MuttVars[idx].flags & R_PAGER_FLOW)
      {
        mutt_set_menu_redraw_full (MENU_PAGER);
        mutt_set_menu_redraw (MENU_PAGER, REDRAW_FLOW);
      }
      if (MuttVars[idx].flags & R_RESORT_SUB)
        set_option (OPTSORTSUBTHREADS);
      if (MuttVars[idx].flags & R_RESORT)
        set_option (OPTNEEDRESORT);
      if (MuttVars[idx].flags & R_RESORT_INIT)
        set_option (OPTRESORTINIT);
      if (MuttVars[idx].flags & R_TREE)
        set_option (OPTREDRAWTREE);
      if (MuttVars[idx].flags & R_REFLOW)
        mutt_reflow_windows ();
#ifdef USE_SIDEBAR
      if (MuttVars[idx].flags & R_SIDEBAR)
        mutt_set_current_menu_redraw (REDRAW_SIDEBAR);
#endif
      if (MuttVars[idx].flags & R_MENU)
        mutt_set_current_menu_redraw_full ();
    }
  }
  return (r);
}

#define MAXERRS 128

/* reads the specified initialization file.  returns -1 if errors were found
   so that we can pause to let the user know...  */
static int source_rc (const char *rcfile, BUFFER *err)
{
  FILE *f;
  int lineno = 0, rc = 0, conv = 0;
  BUFFER *token, *linebuf;
  char *line = NULL;
  char *currentline = NULL;
  size_t linelen;
  pid_t pid;

  dprint (2, (debugfile, "Reading configuration file '%s'.\n",
              rcfile));

  if ((f = mutt_open_read (rcfile, &pid)) == NULL)
  {
    snprintf (err->data, err->dsize, "%s: %s", rcfile, strerror (errno));
    return (-1);
  }

  token = mutt_buffer_pool_get ();
  linebuf = mutt_buffer_pool_get ();

  while ((line = mutt_read_line (line, &linelen, f, &lineno, MUTT_CONT)) != NULL)
  {
    conv=ConfigCharset && Charset;
    if (conv)
    {
      currentline = safe_strdup(line);
      if (!currentline)
        continue;
      mutt_convert_string (&currentline, ConfigCharset, Charset, 0);
    }
    else
      currentline = line;

    mutt_buffer_strcpy (linebuf, currentline);

    if (mutt_parse_rc_buffer (linebuf, token, err) == -1)
    {
      mutt_error (_("Error in %s, line %d: %s"), rcfile, lineno, err->data);
      if (--rc < -MAXERRS)
      {
        if (conv) FREE(&currentline);
        break;
      }
    }
    else
    {
      if (rc < 0)
        rc = -1;
    }
    if (conv)
      FREE(&currentline);
  }

  FREE (&line);
  safe_fclose (&f);
  if (pid != -1)
    mutt_wait_filter (pid);

  if (rc)
  {
    /* the muttrc source keyword */
    snprintf (err->data, err->dsize, rc >= -MAXERRS ? _("source: errors in %s")
              : _("source: reading aborted due to too many errors in %s"), rcfile);
    rc = -1;
  }

  mutt_buffer_pool_release (&token);
  mutt_buffer_pool_release (&linebuf);
  return (rc);
}

#undef MAXERRS

static int parse_run (BUFFER *buf, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  int rc;
  BUFFER *token = NULL;

  if (mutt_extract_token (buf, s, MUTT_TOKEN_LISP) != 0)
  {
    snprintf (err->data, err->dsize, _("source: error at %s"), s->dptr);
    return (-1);
  }
  if (MoreArgs (s))
  {
    strfcpy (err->data, _("run: too many arguments"), err->dsize);
    return (-1);
  }

  token = mutt_buffer_pool_get ();
  rc = mutt_parse_rc_buffer (buf, token, err);
  mutt_buffer_pool_release (&token);

  return rc;
}

static int parse_source (BUFFER *tmp, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  BUFFER *path;
  int rc;

  if (mutt_extract_token (tmp, s, 0) != 0)
  {
    snprintf (err->data, err->dsize, _("source: error at %s"), s->dptr);
    return (-1);
  }
  if (MoreArgs (s))
  {
    strfcpy (err->data, _("source: too many arguments"), err->dsize);
    return (-1);
  }

  path = mutt_buffer_new ();
  mutt_buffer_strcpy (path, tmp->data);
  if (mutt_buffer_len (path) && (*(path->dptr - 1) == '|'))
    mutt_buffer_expand_path_norel (path);
  else
    mutt_buffer_expand_path (path);
  rc = source_rc (mutt_b2s (path), err);
  mutt_buffer_free (&path);

  return rc;
}

int mutt_parse_rc_line (const char *line, BUFFER *err)
{
  BUFFER *line_buffer = NULL, *token = NULL;
  int rc;

  if (!line || !*line)
    return 0;

  line_buffer = mutt_buffer_pool_get ();
  token = mutt_buffer_pool_get ();

  mutt_buffer_strcpy (line_buffer, line);

  rc = mutt_parse_rc_buffer (line_buffer, token, err);

  mutt_buffer_pool_release (&line_buffer);
  mutt_buffer_pool_release (&token);
  return rc;
}

static int parse_cd (BUFFER *tmp, BUFFER *s, union pointer_long_t udata, BUFFER *err)
{
  mutt_extract_token (tmp, s, 0);
  /* norel because could be '..' or other parent directory traversal */
  mutt_buffer_expand_path_norel (tmp);
  if (!mutt_buffer_len (tmp))
  {
    if (Homedir)
      mutt_buffer_strcpy (tmp, Homedir);
    else
    {
      mutt_buffer_strcpy (err, _("too few arguments"));
      return -1;
    }
  }

  if (chdir (mutt_b2s (tmp)) != 0)
  {
    mutt_buffer_printf (err, "cd: %s", strerror (errno));
    return (-1);
  }

  return (0);
}


/* line		command to execute

   token	scratch buffer to be used by parser.
                the reason for this variable is
		to avoid having to allocate and deallocate a lot of memory
		if we are parsing many lines.  the caller can pass in the
		memory to use, which avoids having to create new space for
		every call to this function.

   err		where to write error messages */
int mutt_parse_rc_buffer (BUFFER *line, BUFFER *token, BUFFER *err)
{
  int i, r = -1;

  if (!mutt_buffer_len (line))
    return 0;

  mutt_buffer_clear (err);

  /* Read from the beginning of line->data */
  mutt_buffer_rewind (line);

  SKIPWS (line->dptr);
  while (*line->dptr)
  {
    if (*line->dptr == '#')
      break; /* rest of line is a comment */
    if (*line->dptr == ';')
    {
      line->dptr++;
      continue;
    }
    mutt_extract_token (token, line, 0);
    for (i = 0; Commands[i].name; i++)
    {
      if (!mutt_strcmp (token->data, Commands[i].name))
      {
	if (Commands[i].func (token, line, Commands[i].data, err) != 0)
	  goto finish;
        break;
      }
    }
    if (!Commands[i].name)
    {
      snprintf (err->data, err->dsize, _("%s: unknown command"), NONULL (token->data));
      goto finish;
    }
  }
  r = 0;
finish:
  return (r);
}


#define NUMVARS (sizeof (MuttVars)/sizeof (MuttVars[0]))
#define NUMCOMMANDS (sizeof (Commands)/sizeof (Commands[0]))
/* initial string that starts completion. No telling how much crap
 * the user has typed so far. Allocate LONG_STRING just to be sure! */
static char User_typed [LONG_STRING] = {0};

static int  Num_matched = 0; /* Number of matches for completion */
static char Completed [STRING] = {0}; /* completed string (command or variable) */
static const char **Matches;
/* this is a lie until mutt_init runs: */
static int  Matches_listsize = MAX(NUMVARS,NUMCOMMANDS) + 10;

static void matches_ensure_morespace(int current)
{
  int base_space, extra_space, space;

  if (current > Matches_listsize - 2)
  {
    base_space = MAX(NUMVARS,NUMCOMMANDS) + 1;
    extra_space = Matches_listsize - base_space;
    extra_space *= 2;
    space = base_space + extra_space;
    safe_realloc (&Matches, space * sizeof (char *));
    memset (&Matches[current + 1], 0, space - current);
    Matches_listsize = space;
  }
}

/* helper function for completion.  Changes the dest buffer if
   necessary/possible to aid completion.
	dest == completion result gets here.
	src == candidate for completion.
	try == user entered data for completion.
	len == length of dest buffer.
*/
static void candidate (char *dest, char *try, const char *src, int len)
{
  int l;

  if (strstr (src, try) == src)
  {
    matches_ensure_morespace (Num_matched);
    Matches[Num_matched++] = src;
    if (dest[0] == 0)
      strfcpy (dest, src, len);
    else
    {
      for (l = 0; src[l] && src[l] == dest[l]; l++);
      dest[l] = 0;
    }
  }
}

/* Returns:
 * 2 if the file browser was used.
 *   in this case, the caller needs to redraw.
 * 1 if there is a completion
 * 0 on no completions
 */
int mutt_command_complete (char *buffer, size_t len, int pos, int numtabs)
{
  char *pt = buffer;
  int num;
  int spaces; /* keep track of the number of leading spaces on the line */
  myvar_t *myv;

  SKIPWS (buffer);
  spaces = buffer - pt;

  pt = buffer + pos - spaces;
  while ((pt > buffer) && !isspace ((unsigned char) *pt))
    pt--;

  if (pt == buffer) /* complete cmd */
  {
    /* first TAB. Collect all the matches */
    if (numtabs == 1)
    {
      Num_matched = 0;
      strfcpy (User_typed, pt, sizeof (User_typed));
      memset (Matches, 0, Matches_listsize);
      memset (Completed, 0, sizeof (Completed));
      for (num = 0; Commands[num].name; num++)
	candidate (Completed, User_typed, Commands[num].name, sizeof (Completed));
      matches_ensure_morespace (Num_matched);
      Matches[Num_matched++] = User_typed;

      /* All matches are stored. Longest non-ambiguous string is ""
       * i.e. don't change 'buffer'. Fake successful return this time */
      if (User_typed[0] == 0)
	return 1;
    }

    if (Completed[0] == 0 && User_typed[0])
      return 0;

    /* Num_matched will _always_ be at least 1 since the initial
     * user-typed string is always stored */
    if (numtabs == 1 && Num_matched == 2)
      snprintf(Completed, sizeof(Completed),"%s", Matches[0]);
    else if (numtabs > 1 && Num_matched > 2)
      /* cycle thru all the matches */
      snprintf(Completed, sizeof(Completed), "%s",
	       Matches[(numtabs - 2) % Num_matched]);

    /* return the completed command */
    strncpy (buffer, Completed, len - spaces);
  }
  else if (!mutt_strncmp (buffer, "set", 3)
	   || !mutt_strncmp (buffer, "unset", 5)
	   || !mutt_strncmp (buffer, "reset", 5)
	   || !mutt_strncmp (buffer, "toggle", 6))
  { 		/* complete variables */
    static const char * const prefixes[] = { "no", "inv", "?", "&", 0 };

    pt++;
    /* loop through all the possible prefixes (no, inv, ...) */
    if (!mutt_strncmp (buffer, "set", 3))
    {
      for (num = 0; prefixes[num]; num++)
      {
	if (!mutt_strncmp (pt, prefixes[num], mutt_strlen (prefixes[num])))
	{
	  pt += mutt_strlen (prefixes[num]);
	  break;
	}
      }
    }

    /* first TAB. Collect all the matches */
    if (numtabs == 1)
    {
      Num_matched = 0;
      strfcpy (User_typed, pt, sizeof (User_typed));
      memset (Matches, 0, Matches_listsize);
      memset (Completed, 0, sizeof (Completed));
      for (num = 0; MuttVars[num].option; num++)
	candidate (Completed, User_typed, MuttVars[num].option, sizeof (Completed));
      for (myv = MyVars; myv; myv = myv->next)
	candidate (Completed, User_typed, myv->name, sizeof (Completed));
      matches_ensure_morespace (Num_matched);
      Matches[Num_matched++] = User_typed;

      /* All matches are stored. Longest non-ambiguous string is ""
       * i.e. don't change 'buffer'. Fake successful return this time */
      if (User_typed[0] == 0)
	return 1;
    }

    if (Completed[0] == 0 && User_typed[0])
      return 0;

    /* Num_matched will _always_ be at least 1 since the initial
     * user-typed string is always stored */
    if (numtabs == 1 && Num_matched == 2)
      snprintf(Completed, sizeof(Completed),"%s", Matches[0]);
    else if (numtabs > 1 && Num_matched > 2)
      /* cycle thru all the matches */
      snprintf(Completed, sizeof(Completed), "%s",
	       Matches[(numtabs - 2) % Num_matched]);

    strncpy (pt, Completed, buffer + len - pt - spaces);
  }
  else if (!mutt_strncmp (buffer, "exec", 4))
  {
    const struct menu_func_op_t *menu = km_get_table (CurrentMenu);

    if (!menu && CurrentMenu != MENU_PAGER)
      menu = OpGeneric;

    pt++;
    /* first TAB. Collect all the matches */
    if (numtabs == 1)
    {
      Num_matched = 0;
      strfcpy (User_typed, pt, sizeof (User_typed));
      memset (Matches, 0, Matches_listsize);
      memset (Completed, 0, sizeof (Completed));
      for (num = 0; menu[num].name; num++)
	candidate (Completed, User_typed, menu[num].name, sizeof (Completed));
      /* try the generic menu */
      if (CurrentMenu != MENU_PAGER && CurrentMenu != MENU_GENERIC)
      {
	menu = OpGeneric;
	for (num = 0; menu[num].name; num++)
	  candidate (Completed, User_typed, menu[num].name, sizeof (Completed));
      }
      matches_ensure_morespace (Num_matched);
      Matches[Num_matched++] = User_typed;

      /* All matches are stored. Longest non-ambiguous string is ""
       * i.e. don't change 'buffer'. Fake successful return this time */
      if (User_typed[0] == 0)
	return 1;
    }

    if (Completed[0] == 0 && User_typed[0])
      return 0;

    /* Num_matched will _always_ be at least 1 since the initial
     * user-typed string is always stored */
    if (numtabs == 1 && Num_matched == 2)
      snprintf(Completed, sizeof(Completed),"%s", Matches[0]);
    else if (numtabs > 1 && Num_matched > 2)
      /* cycle thru all the matches */
      snprintf(Completed, sizeof(Completed), "%s",
	       Matches[(numtabs - 2) % Num_matched]);

    strncpy (pt, Completed, buffer + len - pt - spaces);
  }
  else if (!mutt_strncmp (buffer, "cd", 2))
  {
    pt = buffer + 2;
    SKIPWS (pt);
    if (numtabs == 1)
    {
      if (mutt_complete (pt, buffer + len - pt - spaces))
        return 0;
    }
    else
    {
      BUFFER *selectbuf;
      char keybuf[SHORT_STRING];

      if (!km_expand_key (keybuf, sizeof(keybuf),
                          km_find_func (MENU_FOLDER, OP_BROWSER_VIEW_FILE)) ||
          keybuf[0] == '\0')
      {
        strcpy (keybuf, "<view-file>");  /* __STRCPY_CHECKED__ */
      }
      /* L10N:
         When tab completing the :cd path argument, the folder browser
         will be invoked upon the second tab.  This message will be
         printed below the folder browser, to instruct the user how to
         select a directory for completion.

         %s will print the key bound to <view-file>, which is
         '<Space>' by default.  If no keys are bound to <view-file>
         then %s will print the function name: '<view-file>'.
       */
      mutt_message (_("Use '%s' to select a directory"), keybuf);

      selectbuf = mutt_buffer_pool_get ();
      mutt_buffer_strcpy (selectbuf, pt);
      mutt_buffer_select_file (selectbuf, MUTT_SEL_DIRECTORY);
      if (mutt_buffer_len (selectbuf))
        strfcpy (pt, mutt_b2s (selectbuf), buffer + len - pt - spaces);
      mutt_buffer_pool_release (&selectbuf);
      return 2;
    }
  }
  else
    return 0;

  return 1;
}

int mutt_var_value_complete (char *buffer, size_t len, int pos)
{
  char var[STRING], *pt = buffer;
  int spaces, rv = 0;

  if (buffer[0] == 0)
    return 0;

  SKIPWS (buffer);
  spaces = buffer - pt;

  pt = buffer + pos - spaces;
  while ((pt > buffer) && !isspace ((unsigned char) *pt))
    pt--;
  pt++; /* move past the space */
  if (*pt == '=') /* abort if no var before the '=' */
    return 0;

  if (mutt_strncmp (buffer, "set", 3) == 0)
  {
    int idx;
    const char *myvarval;

    strfcpy (var, pt, sizeof (var));
    /* ignore the trailing '=' when comparing */
    var[mutt_strlen (var) - 1] = 0;
    if ((idx = mutt_option_index (var)) == -1)
    {
      if ((myvarval = myvar_get(var)) != NULL)
      {
	pretty_var (pt, len - (pt - buffer), var, myvarval);
	rv = 1;
      }
    }
    else
    {
      BUFFER *val = mutt_buffer_pool_get ();

      if (var_to_string (idx, val))
      {
        pretty_var (pt, len - (pt - buffer), var, mutt_b2s (val));
        rv = 1;
      }
      mutt_buffer_pool_release (&val);
    }
  }
  return rv;
}

static int var_to_string (int idx, BUFFER *val)
{
  static const char * const vals[] = { "no", "yes", "ask-no", "ask-yes" };

  mutt_buffer_clear (val);
  mutt_buffer_increase_size (val, LONG_STRING);

  if ((DTYPE(MuttVars[idx].type) == DT_STR) ||
      (DTYPE(MuttVars[idx].type) == DT_RX))
  {
    mutt_buffer_strcpy (val, NONULL (*((char **) MuttVars[idx].data.p)));
  }
  else if (DTYPE (MuttVars[idx].type) == DT_PATH ||
           DTYPE (MuttVars[idx].type) == DT_CMD_PATH)
  {
    mutt_buffer_strcpy (val, NONULL (*((char **) MuttVars[idx].data.p)));
    if (mutt_strcmp (MuttVars[idx].option, "record") == 0)
      mutt_buffer_pretty_multi_mailbox (val, FccDelimiter);
    else
      mutt_buffer_pretty_mailbox (val);
  }
  else if (DTYPE (MuttVars[idx].type) == DT_MBCHARTBL)
  {
    mbchar_table *mbt = (*((mbchar_table **) MuttVars[idx].data.p));
    if (mbt)
      mutt_buffer_strcpy (val, NONULL (mbt->orig_str));
  }
  else if (DTYPE (MuttVars[idx].type) == DT_ADDR)
  {
    rfc822_write_address (val->data, val->dsize, *((ADDRESS **) MuttVars[idx].data.p), 0);
    mutt_buffer_fix_dptr (val);
    /* XXX: this is a hack until we have buffer address writing */
    if (mutt_buffer_len (val) + 1 == val->dsize)
    {
      mutt_buffer_clear (val);
      mutt_buffer_increase_size (val, HUGE_STRING);
      rfc822_write_address (val->data, val->dsize, *((ADDRESS **) MuttVars[idx].data.p), 0);
      mutt_buffer_fix_dptr (val);
    }
  }
  else if (DTYPE (MuttVars[idx].type) == DT_QUAD)
    mutt_buffer_strcpy (val, vals[quadoption (MuttVars[idx].data.l)]);
  else if (DTYPE (MuttVars[idx].type) == DT_NUM)
  {
    short sval = *((short *) MuttVars[idx].data.p);

    /* avert your eyes, gentle reader */
    if (mutt_strcmp (MuttVars[idx].option, "wrapmargin") == 0)
      sval = sval > 0 ? 0 : -sval;

    mutt_buffer_printf (val, "%d", sval);
  }
  else if (DTYPE (MuttVars[idx].type) == DT_LNUM)
  {
    long sval = *((long *) MuttVars[idx].data.p);

    mutt_buffer_printf (val, "%ld", sval);
  }
  else if (DTYPE (MuttVars[idx].type) == DT_SORT)
  {
    const struct mapping_t *map;
    const char *p;

    switch (MuttVars[idx].type & DT_SUBTYPE_MASK)
    {
      case DT_SORT_ALIAS:
        map = SortAliasMethods;
        break;
      case DT_SORT_BROWSER:
        map = SortBrowserMethods;
        break;
      case DT_SORT_KEYS:
        if ((WithCrypto & APPLICATION_PGP))
          map = SortKeyMethods;
        else
          map = SortMethods;
        break;
      case DT_SORT_THREAD_GROUPS:
        map = SortThreadGroupsMethods;
        break;
      default:
        map = SortMethods;
        break;
    }
    p = mutt_getnamebyvalue (*((short *) MuttVars[idx].data.p) & SORT_MASK, map);
    mutt_buffer_printf (val, "%s%s%s",
              (*((short *) MuttVars[idx].data.p) & SORT_REVERSE) ? "reverse-" : "",
              (*((short *) MuttVars[idx].data.p) & SORT_LAST) ? "last-" : "",
              p);
  }
  else if (DTYPE (MuttVars[idx].type) == DT_MAGIC)
  {
    char *p;

    switch (DefaultMagic)
    {
      case MUTT_MBOX:
        p = "mbox";
        break;
      case MUTT_MMDF:
        p = "MMDF";
        break;
      case MUTT_MH:
        p = "MH";
        break;
      case MUTT_MAILDIR:
        p = "Maildir";
        break;
      default:
        p = "unknown";
    }
    mutt_buffer_strcpy (val, p);
  }
  else if (DTYPE (MuttVars[idx].type) == DT_BOOL)
    mutt_buffer_strcpy (val, option (MuttVars[idx].data.l) ? "yes" : "no");
  else
    return 0;

  return 1;
}

/* Implement the -Q command line flag */
int mutt_query_variables (LIST *queries)
{
  LIST *p;

  char command[STRING];

  BUFFER err;

  mutt_buffer_init (&err);

  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);

  for (p = queries; p; p = p->next)
  {
    snprintf (command, sizeof (command), "set ?%s\n", p->data);
    if (mutt_parse_rc_line (command, &err) == -1)
    {
      fprintf (stderr, "%s\n", err.data);
      FREE (&err.data);

      return 1;
    }
    printf ("%s\n", err.data);
  }

  FREE (&err.data);

  return 0;
}

/* dump out the value of all the variables we have */
int mutt_dump_variables (void)
{
  int i;

  char command[STRING];

  BUFFER err;

  mutt_buffer_init (&err);

  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);

  for (i = 0; MuttVars[i].option; i++)
  {
    if (MuttVars[i].type == DT_SYN)
      continue;

    snprintf (command, sizeof (command), "set ?%s\n", MuttVars[i].option);
    if (mutt_parse_rc_line (command, &err) == -1)
    {
      fprintf (stderr, "%s\n", err.data);
      FREE (&err.data);

      return 1;
    }
    printf("%s\n", err.data);
  }

  FREE (&err.data);

  return 0;
}

const char *mutt_getnamebyvalue (int val, const struct mapping_t *map)
{
  int i;

  for (i=0; map[i].name; i++)
    if (map[i].value == val)
      return (map[i].name);
  return NULL;
}

const struct mapping_t *mutt_get_mapentry_by_name (const char *name,
                                                  const struct mapping_t *map)
{
  int i;

  for (i = 0; map[i].name; i++)
    if (ascii_strcasecmp (map[i].name, name) == 0)
      return &map[i];
  return NULL;
}

int mutt_getvaluebyname (const char *name, const struct mapping_t *map)
{
  const struct mapping_t *entry = mutt_get_mapentry_by_name (name, map);
  if (entry)
    return entry->value;
  return -1;
}

#ifdef DEBUG
static void start_debug (int rotate)
{
  int i;
  BUFFER *buf, *buf2;

  buf = mutt_buffer_pool_get ();
  buf2 = mutt_buffer_pool_get ();

  /* rotate the old debug logs */
  if (rotate)
  {
    for (i=3; i>=0; i--)
    {
      mutt_buffer_printf (buf, "%s/.muttdebug%d", NONULL(Homedir), i);
      mutt_buffer_printf (buf2, "%s/.muttdebug%d", NONULL(Homedir), i+1);
      rename (mutt_b2s (buf), mutt_b2s (buf2));
    }
    debugfile = safe_fopen(mutt_b2s (buf), "w");
  }
  else
  {
    mutt_buffer_printf (buf, "%s/.muttdebug0", NONULL(Homedir));
    debugfile = safe_fopen(mutt_b2s (buf), "a");
  }

  if (debugfile != NULL)
  {
    setbuf (debugfile, NULL); /* don't buffer the debugging output! */
    dprint(1,(debugfile,"Mutt/%s (%s) debugging at level %d\n",
	      MUTT_VERSION, ReleaseDate, debuglevel));
  }

  mutt_buffer_pool_release (&buf);
  mutt_buffer_pool_release (&buf2);
}
#endif

static int mutt_execute_commands (LIST *p)
{
  BUFFER err;

  mutt_buffer_init (&err);
  err.dsize = STRING;
  err.data = safe_malloc (err.dsize);
  for (; p; p = p->next)
  {
    if (mutt_parse_rc_line (p->data, &err) != 0)
    {
      fprintf (stderr, _("Error in command line: %s\n"), err.data);
      FREE (&err.data);

      return -1;
    }
  }
  FREE (&err.data);

  return 0;
}

static char* mutt_find_cfg (const char *home, const char *xdg_cfg_home)
{
  const char* names[] =
    {
      "muttrc-" MUTT_VERSION,
      "muttrc",
      NULL,
    };

  const char* locations[][2] =
    {
      { home, ".", },
      { home, ".mutt/" },
      { xdg_cfg_home, "mutt/", },
      { NULL, NULL },
    };

  int i;

  for (i = 0; locations[i][0] || locations[i][1]; i++)
  {
    int j;

    if (!locations[i][0])
      continue;

    for (j = 0; names[j]; j++)
    {
      char buffer[STRING];

      snprintf (buffer, sizeof (buffer),
                "%s/%s%s", locations[i][0], locations[i][1], names[j]);
      if (access (buffer, F_OK) == 0)
        return safe_strdup(buffer);
    }
  }

  return NULL;
}

void mutt_init (int skip_sys_rc, LIST *commands)
{
  struct passwd *pw;
  struct utsname utsname;
  char *p;
  char *domain = NULL;
  int i, need_pause = 0;
  BUFFER err, *buffer = NULL;

  mutt_buffer_init (&err);
  mutt_buffer_increase_size (&err, STRING);

  Groups = hash_create (1031, 0);
  /* reverse alias keys need to be strdup'ed because of idna conversions */
  ReverseAlias = hash_create (1031, MUTT_HASH_STRCASECMP | MUTT_HASH_STRDUP_KEYS |
                              MUTT_HASH_ALLOW_DUPS);

  mutt_menu_init ();
  mutt_buffer_pool_init ();

  buffer = mutt_buffer_pool_get ();

  /*
   * XXX - use something even more difficult to predict?
   */
  snprintf (AttachmentMarker, sizeof (AttachmentMarker),
	    "\033]9;%ld\a", (long) time (NULL));
  snprintf (ProtectedHeaderMarker, sizeof (ProtectedHeaderMarker),
	    "\033]8;%ld\a", (long) time (NULL));

  /* on one of the systems I use, getcwd() does not return the same prefix
     as is listed in the passwd file */
  if ((p = getenv ("HOME")))
    Homedir = safe_strdup (p);

  /* Get some information about the user */
  if ((pw = getpwuid (getuid ())))
  {
    char rnbuf[STRING];

    Username = safe_strdup (pw->pw_name);
    if (!Homedir)
      Homedir = safe_strdup (pw->pw_dir);

    Realname = safe_strdup (mutt_gecos_name (rnbuf, sizeof (rnbuf), pw));
    Shell = safe_strdup (pw->pw_shell);
    endpwent ();
  }
  else
  {
    if (!Homedir)
    {
      mutt_endwin (NULL);
      fputs (_("unable to determine home directory"), stderr);
      exit (1);
    }
    if ((p = getenv ("USER")))
      Username = safe_strdup (p);
    else
    {
      mutt_endwin (NULL);
      fputs (_("unable to determine username"), stderr);
      exit (1);
    }
    Shell = safe_strdup ((p = getenv ("SHELL")) ? p : "/bin/sh");
  }

#ifdef DEBUG
  /* Start up debugging mode if requested */
  if (debuglevel > 0)
    start_debug (1);
  if (debuglevel < 0)
  {
    debuglevel = -debuglevel;
    start_debug (0);
  }
#endif


  /*
   * Determine Hostname.
   *
   * This is used in tempfile creation, so set it early.  We delay
   * Fqdn ($hostname) setting until the muttrc is evaluated, so the
   * user has the ability to manually set it (for example, if their
   * DNS resolution has issues).
   */

  /*
   * The call to uname() shouldn't fail, but if it does, the system is horribly
   * broken, and the system's networking configuration is in an unreliable
   * state.  We should bail.
   */
  if ((uname (&utsname)) == -1)
  {
    mutt_endwin (NULL);
    perror (_("unable to determine nodename via uname()"));
    exit (1);
  }

  /* some systems report the FQDN instead of just the hostname */
  if ((p = strchr (utsname.nodename, '.')))
    Hostname = mutt_substrdup (utsname.nodename, p);
  else
    Hostname = safe_strdup (utsname.nodename);


  if ((p = getenv ("MAIL")))
    Spoolfile = safe_strdup (p);
  else if ((p = getenv ("MAILDIR")))
    Spoolfile = safe_strdup (p);
  else
  {
#ifdef HOMESPOOL
    mutt_buffer_concat_path (buffer, NONULL (Homedir), MAILPATH);
#else
    mutt_buffer_concat_path (buffer, MAILPATH, NONULL(Username));
#endif
    Spoolfile = safe_strdup (mutt_b2s (buffer));
  }

  if ((p = getenv ("MAILCAPS")))
    MailcapPath = safe_strdup (p);
  else
  {
    /* Default search path from RFC1524 */
    MailcapPath = safe_strdup ("~/.mailcap:" PKGDATADIR "/mailcap:" SYSCONFDIR "/mailcap:/etc/mailcap:/usr/etc/mailcap:/usr/local/etc/mailcap");
  }

  Tempdir = safe_strdup ((p = getenv ("TMPDIR")) ? p : "/tmp");

  p = getenv ("VISUAL");
  if (!p)
  {
    p = getenv ("EDITOR");
    if (!p)
      p = "vi";
  }
  Editor = safe_strdup (p);
  Visual = safe_strdup (p);

  if ((p = getenv ("REPLYTO")) != NULL)
  {
    mutt_buffer_printf (buffer, "Reply-To: %s", p);
    update_my_hdr (mutt_b2s (buffer));
  }

  if ((p = getenv ("EMAIL")) != NULL)
    From = rfc822_parse_adrlist (NULL, p);

  mutt_set_langinfo_charset ();
  mutt_set_charset (Charset);

  Matches = safe_calloc (Matches_listsize, sizeof (char *));

  /* Set standard defaults */
  for (i = 0; MuttVars[i].option; i++)
  {
    mutt_set_default (&MuttVars[i]);
    mutt_restore_default (&MuttVars[i]);
  }

  CurrentMenu = MENU_MAIN;


#ifndef LOCALES_HACK
  /* Do we have a locale definition? */
  if (((p = getenv ("LC_ALL")) != NULL && p[0]) ||
      ((p = getenv ("LANG")) != NULL && p[0]) ||
      ((p = getenv ("LC_CTYPE")) != NULL && p[0]))
    set_option (OPTLOCALES);
#endif

#ifdef HAVE_GETSID
  /* Unset suspend by default if we're the session leader */
  if (getsid(0) == getpid())
    unset_option (OPTSUSPEND);
#endif

  mutt_init_history ();
  mutt_error_history_init ();

  /* RFC2368, "4. Unsafe headers"
   * The creator of a mailto URL cannot expect the resolver of a URL to
   * understand more than the "subject" and "body" headers. Clients that
   * resolve mailto URLs into mail messages should be able to correctly
   * create RFC 822-compliant mail messages using the "subject" and "body"
   * headers.
   */
  add_to_list(&MailtoAllow, "body");
  add_to_list(&MailtoAllow, "subject");

  /* Allow a few other commonly used headers for mailing list
   * software, and platforms such as Sourcehut.
   */
  add_to_list(&MailtoAllow, "cc");
  add_to_list(&MailtoAllow, "in-reply-to");
  add_to_list(&MailtoAllow, "references");

  if (!Muttrc)
  {
    const char *xdg_cfg_home = getenv ("XDG_CONFIG_HOME");

    if (!xdg_cfg_home && Homedir)
    {
      mutt_buffer_printf (buffer, "%s/.config", Homedir);
      xdg_cfg_home = mutt_b2s (buffer);
    }

    Muttrc = mutt_find_cfg (Homedir, xdg_cfg_home);
  }
  else
  {
    mutt_buffer_strcpy (buffer, Muttrc);
    FREE (&Muttrc);
    mutt_buffer_expand_path (buffer);
    Muttrc = safe_strdup (mutt_b2s (buffer));
    if (access (Muttrc, F_OK))
    {
      mutt_buffer_printf (buffer, "%s: %s", Muttrc, strerror (errno));
      mutt_endwin (mutt_b2s (buffer));
      exit (1);
    }
  }

  if (Muttrc)
  {
    FREE (&AliasFile);
    AliasFile = safe_strdup (Muttrc);
  }

  /* Process the global rc file if it exists and the user hasn't explicitly
     requested not to via "-n".  */
  if (!skip_sys_rc)
  {
    mutt_buffer_printf (buffer, "%s/Muttrc-%s", SYSCONFDIR, MUTT_VERSION);
    if (access (mutt_b2s (buffer), F_OK) == -1)
      mutt_buffer_printf (buffer, "%s/Muttrc", SYSCONFDIR);
    if (access (mutt_b2s (buffer), F_OK) == -1)
      mutt_buffer_printf (buffer, "%s/Muttrc-%s", PKGDATADIR, MUTT_VERSION);
    if (access (mutt_b2s (buffer), F_OK) == -1)
      mutt_buffer_printf (buffer, "%s/Muttrc", PKGDATADIR);
    if (access (mutt_b2s (buffer), F_OK) != -1)
    {
      if (source_rc (mutt_b2s (buffer), &err) != 0)
      {
	fputs (err.data, stderr);
	fputc ('\n', stderr);
	need_pause = 1;
      }
    }
  }

  /* Read the user's initialization file.  */
  if (Muttrc)
  {
    if (!option (OPTNOCURSES))
      endwin ();
    if (source_rc (Muttrc, &err) != 0)
    {
      fputs (err.data, stderr);
      fputc ('\n', stderr);
      need_pause = 1;
    }
  }

  if (mutt_execute_commands (commands) != 0)
    need_pause = 1;

  if (need_pause && !option (OPTNOCURSES))
  {
    if (mutt_any_key_to_continue (NULL) == -1)
      mutt_exit(1);
  }


  /* If not set in the muttrc or mutt_execute_commands(), set Fqdn ($hostname).
   * Use configured domain first, DNS next, then uname
   */
  if (!Fqdn)
  {
    dprint (1, (debugfile, "Setting $hostname\n"));
#ifdef DOMAIN
    domain = safe_strdup (DOMAIN);
#endif /* DOMAIN */

    if (domain)
    {
      /* we have a compile-time domain name, use that for Fqdn */
      Fqdn = safe_malloc (mutt_strlen (domain) + mutt_strlen (Hostname) + 2);
      sprintf (Fqdn, "%s.%s", NONULL(Hostname), domain);	/* __SPRINTF_CHECKED__ */
    }
    else if (!(getdnsdomainname (buffer)))
    {
      Fqdn = safe_malloc (mutt_buffer_len (buffer) + mutt_strlen (Hostname) + 2);
      sprintf (Fqdn, "%s.%s", NONULL(Hostname), mutt_b2s (buffer));	/* __SPRINTF_CHECKED__ */
    }
    else
      /*
       * DNS failed, use the nodename.  Whether or not the nodename had a '.' in
       * it, we can use the nodename as the FQDN.  On hosts where DNS is not
       * being used, e.g. small network that relies on hosts files, a short host
       * name is all that is required for SMTP to work correctly.  It could be
       * wrong, but we've done the best we can, at this point the onus is on the
       * user to provide the correct hostname if the nodename won't work in their
       * network.
       */
      Fqdn = safe_strdup(utsname.nodename);
    dprint (1, (debugfile, "$hostname set to \"%s\"\n", NONULL (Fqdn)));
  }


  mutt_read_histfile ();

  FREE (&err.data);
  mutt_buffer_pool_release (&buffer);
}

int mutt_get_hook_type (const char *name)
{
  const struct command_t *c;

  for (c = Commands ; c->name ; c++)
    if ((c->func == mutt_parse_hook || c->func == mutt_parse_idxfmt_hook) &&
        ascii_strcasecmp (c->name, name) == 0)
      return c->data.l;
  return 0;
}

static int parse_group_context (group_context_t **ctx, BUFFER *buf, BUFFER *s, BUFFER *err)
{
  while (!mutt_strcasecmp (buf->data, "-group"))
  {
    if (!MoreArgs (s))
    {
      strfcpy (err->data, _("-group: no group name"), err->dsize);
      goto bail;
    }

    mutt_extract_token (buf, s, 0);

    mutt_group_context_add (ctx, mutt_pattern_group (buf->data));

    if (!MoreArgs (s))
    {
      strfcpy (err->data, _("out of arguments"), err->dsize);
      goto bail;
    }

    mutt_extract_token (buf, s, 0);
  }

  return 0;

bail:
  mutt_group_context_destroy (ctx);
  return -1;
}

static void myvar_set (const char* var, const char* val)
{
  myvar_t** cur;

  for (cur = &MyVars; *cur; cur = &((*cur)->next))
    if (!mutt_strcmp ((*cur)->name, var))
      break;

  if (!*cur)
    *cur = safe_calloc (1, sizeof (myvar_t));

  if (!(*cur)->name)
    (*cur)->name = safe_strdup (var);

  mutt_str_replace (&(*cur)->value, val);
}

static void myvar_del (const char* var)
{
  myvar_t **cur;
  myvar_t *tmp;


  for (cur = &MyVars; *cur; cur = &((*cur)->next))
    if (!mutt_strcmp ((*cur)->name, var))
      break;

  if (*cur)
  {
    tmp = (*cur)->next;
    FREE (&(*cur)->name);
    FREE (&(*cur)->value);
    FREE (cur);		/* __FREE_CHECKED__ */
    *cur = tmp;
  }
}

static const char* myvar_get (const char* var)
{
  myvar_t* cur;

  for (cur = MyVars; cur; cur = cur->next)
    if (!mutt_strcmp (cur->name, var))
      return NONULL(cur->value);

  return NULL;
}

int mutt_label_complete (char *buffer, size_t len, int numtabs)
{
  char *pt = buffer;
  int spaces; /* keep track of the number of leading spaces on the line */

  if (!Context || !Context->label_hash)
    return 0;

  SKIPWS (buffer);
  spaces = buffer - pt;

  /* first TAB. Collect all the matches */
  if (numtabs == 1)
  {
    struct hash_elem *entry;
    struct hash_walk_state state;

    Num_matched = 0;
    strfcpy (User_typed, buffer, sizeof (User_typed));
    memset (Matches, 0, Matches_listsize);
    memset (Completed, 0, sizeof (Completed));
    memset (&state, 0, sizeof(state));
    while ((entry = hash_walk(Context->label_hash, &state)))
      candidate (Completed, User_typed, entry->key.strkey, sizeof (Completed));
    matches_ensure_morespace (Num_matched);
    qsort(Matches, Num_matched, sizeof(char *), (sort_t *) mutt_strcasecmp);
    Matches[Num_matched++] = User_typed;

    /* All matches are stored. Longest non-ambiguous string is ""
     * i.e. dont change 'buffer'. Fake successful return this time */
    if (User_typed[0] == 0)
      return 1;
  }

  if (Completed[0] == 0 && User_typed[0])
    return 0;

  /* Num_matched will _always_ be atleast 1 since the initial
   * user-typed string is always stored */
  if (numtabs == 1 && Num_matched == 2)
    snprintf(Completed, sizeof(Completed), "%s", Matches[0]);
  else if (numtabs > 1 && Num_matched > 2)
    /* cycle thru all the matches */
    snprintf(Completed, sizeof(Completed), "%s",
             Matches[(numtabs - 2) % Num_matched]);

  /* return the completed label */
  strncpy (buffer, Completed, len - spaces);

  return 1;
}
