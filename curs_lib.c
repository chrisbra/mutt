/*
 * Copyright (C) 1996-2002,2010,2012-2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
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
#include "mutt_curses.h"
#include "pager.h"
#include "mbyte.h"
#include "background.h"
#ifdef USE_INOTIFY
#include "monitor.h"
#endif

#include <termios.h>
#include <sys/types.h>
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ctype.h>
#ifdef HAVE_SYS_TIME_H
# include <sys/time.h>
#endif
#include <time.h>

#ifdef HAVE_LANGINFO_YESEXPR
#include <langinfo.h>
#endif

/* Error message ring */
struct error_history
{
  char **msg;
  short last;
} ErrorHistory = {0, 0};

static short OldErrorHistSize = 0;


/* not possible to unget more than one char under some curses libs, and it
 * is impossible to unget function keys in SLang, so roll our own input
 * buffering routines.
 */

/* These are used for macros and exec/push commands.
 * They can be temporarily ignored by setting OPTIGNOREMACROEVENTS
 */
static size_t MacroBufferCount = 0;
static size_t MacroBufferLen = 0;
static event_t *MacroEvents;

/* These are used in all other "normal" situations, and are not
 * ignored when setting OPTIGNOREMACROEVENTS
 */
static size_t UngetCount = 0;
static size_t UngetLen = 0;
static event_t *UngetKeyEvents;

int MuttGetchTimeout = -1;

mutt_window_t *MuttHelpWindow = NULL;
mutt_window_t *MuttIndexWindow = NULL;
mutt_window_t *MuttStatusWindow = NULL;
mutt_window_t *MuttMessageWindow = NULL;
#ifdef USE_SIDEBAR
mutt_window_t *MuttSidebarWindow = NULL;
#endif

static void reflow_message_window_rows (int mw_rows);


void mutt_refresh (void)
{
  /* don't refresh when we are waiting for a child. */
  if (option (OPTKEEPQUIET))
    return;

  /* don't refresh in the middle of macros unless necessary */
  if (MacroBufferCount && !option (OPTFORCEREFRESH) &&
      !option (OPTIGNOREMACROEVENTS))
    return;

  /* else */
  refresh ();
}

/* Make sure that the next refresh does a full refresh.  This could be
   optimized by not doing it at all if DISPLAY is set as this might
   indicate that a GUI based pinentry was used.  Having an option to
   customize this is of course the Mutt way.  */
void mutt_need_hard_redraw (void)
{
  keypad (stdscr, TRUE);
  clearok (stdscr, TRUE);
  mutt_set_current_menu_redraw_full ();
}

/* delay is just like for timeout() or poll():
 *   the number of milliseconds mutt_getch() should block for input.
 *   delay == 0 means mutt_getch() is non-blocking.
 *   delay < 0 means mutt_getch is blocking.
 */
void mutt_getch_timeout (int delay)
{
  MuttGetchTimeout = delay;
  timeout (delay);
}

#ifdef USE_INOTIFY
static int mutt_monitor_getch (void)
{
  int ch;

  /* ncurses has its own internal buffer, so before we perform a poll,
   * we need to make sure there isn't a character waiting */
  timeout (0);
  ch = getch ();
  timeout (MuttGetchTimeout);
  if (ch == ERR)
  {
    if (mutt_monitor_poll () != 0)
      ch = ERR;
    else
      ch = getch ();
  }
  return ch;
}
#endif /* USE_INOTIFY */

event_t mutt_getch (void)
{
  int ch;
  event_t err = {-1, OP_NULL }, ret;
  event_t timeout = {-2, OP_NULL};

  if (UngetCount)
    return (UngetKeyEvents[--UngetCount]);

  if (!option(OPTIGNOREMACROEVENTS) && MacroBufferCount)
    return (MacroEvents[--MacroBufferCount]);

  SigInt = 0;

  mutt_allow_interrupt (1);
#ifdef KEY_RESIZE
  /* ncurses 4.2 sends this when the screen is resized */
  ch = KEY_RESIZE;
  while (ch == KEY_RESIZE)
#endif /* KEY_RESIZE */
#ifdef USE_INOTIFY
    ch = mutt_monitor_getch ();
#else
    ch = getch ();
#endif /* USE_INOTIFY */
  mutt_allow_interrupt (0);

  if (SigInt)
  {
    mutt_query_exit ();
    return err;
  }

  /* either timeout, a sigwinch (if timeout is set), or the terminal
   * has been lost */
  if (ch == ERR)
  {
    if (!isatty (0))
    {
      endwin ();
      exit (1);
    }
    return timeout;
  }

  if ((ch & 0x80) && option (OPTMETAKEY))
  {
    /* send ALT-x as ESC-x */
    ch &= ~0x80;
    mutt_unget_event (ch, 0);
    ret.ch = '\033';
    ret.op = 0;
    return ret;
  }

  ret.ch = ch;
  ret.op = 0;
  return (ch == ctrl ('G') ? err : ret);
}

static int _get_field (const char *field, BUFFER *buffer, int complete,
                       int multiple, char ***files, int *numfiles)
{
  int ret;
  int x;

  ENTER_STATE *es = mutt_new_enter_state();

  do
  {
#if defined (USE_SLANG_CURSES) || defined (HAVE_RESIZETERM)
    if (SigWinch)
    {
      SigWinch = 0;
      mutt_resize_screen ();
      clearok(stdscr, TRUE);
      mutt_current_menu_redraw ();
    }
#endif
    mutt_window_clearline (MuttMessageWindow, 0);
    SETCOLOR (MT_COLOR_PROMPT);
    addstr ((char *)field); /* cast to get around bad prototypes */
    NORMAL_COLOR;
    mutt_refresh ();
    mutt_window_getyx (MuttMessageWindow, NULL, &x);
    ret = _mutt_enter_string (buffer->data, buffer->dsize, x, complete, multiple, files, numfiles, es);
  }
  while (ret == 1);

  if (ret != 0)
    mutt_buffer_clear (buffer);
  else
    mutt_buffer_fix_dptr (buffer);

  mutt_window_clearline (MuttMessageWindow, 0);
  mutt_free_enter_state (&es);

  return (ret);
}

int mutt_get_field (const char *field, char *buf, size_t buflen, int complete)
{
  BUFFER *buffer;
  int rc;

  buffer = mutt_buffer_pool_get ();

  mutt_buffer_increase_size (buffer, buflen);
  mutt_buffer_addstr (buffer, buf);
  rc = _get_field (field, buffer, complete, 0, NULL, NULL);
  strfcpy (buf, mutt_b2s (buffer), buflen);

  mutt_buffer_pool_release (&buffer);
  return rc;
}

int mutt_buffer_get_field (const char *field, BUFFER *buffer, int complete)
{
  return _get_field (field, buffer, complete, 0, NULL, NULL);
}

int mutt_get_field_unbuffered (char *msg, char *buf, size_t buflen, int flags)
{
  int rc, reset_ignoremacro = 0;

  if (!option (OPTIGNOREMACROEVENTS))
  {
    set_option (OPTIGNOREMACROEVENTS);
    reset_ignoremacro = 1;
  }
  rc = mutt_get_field (msg, buf, buflen, flags);
  if (reset_ignoremacro)
    unset_option (OPTIGNOREMACROEVENTS);

  return (rc);
}

void mutt_clear_error (void)
{
  Errorbuf[0] = 0;
  if (!option(OPTNOCURSES))
    mutt_window_clearline (MuttMessageWindow, 0);
}

void mutt_edit_file (const char *editor, const char *data)
{
  BUFFER *cmd;

  cmd = mutt_buffer_pool_get ();

  mutt_endwin (NULL);
  mutt_expand_file_fmt (cmd, editor, data);
  if (mutt_system (mutt_b2s (cmd)))
  {
    mutt_error (_("Error running \"%s\"!"), mutt_b2s (cmd));
    mutt_sleep (2);
  }

  mutt_buffer_pool_release (&cmd);
}

/* Prompts for a yes or no response.
 * If var is non-null, it will print a help message referencing the variable
 * when '?' is pressed.
 */
int mutt_yesorno_with_help (const char *msg, int def, const char *var)
{
  event_t ch;
  char *yes = _("yes");
  char *no = _("no");
  BUFFER *answer_buffer = NULL, *help_buffer = NULL;
  int answer_string_wid, msg_wid;
  size_t trunc_msg_len, trunc_help_len;
  int redraw = 1, prompt_lines = 1;
  int show_help_prompt = 0, show_help = 0;

#ifdef HAVE_LANGINFO_YESEXPR
  char *expr;
  regex_t reyes;
  regex_t reno;
  int reyes_ok;
  int reno_ok;
  char answer[2];

  answer[1] = 0;

  reyes_ok = (expr = nl_langinfo (YESEXPR)) && expr[0] == '^' &&
    !REGCOMP (&reyes, expr, REG_NOSUB);
  reno_ok = (expr = nl_langinfo (NOEXPR)) && expr[0] == '^' &&
    !REGCOMP (&reno, expr, REG_NOSUB);
#endif

  if (var)
    show_help_prompt = 1;

  /*
   * In order to prevent the default answer to the question to wrapped
   * around the screen in the even the question is wider than the screen,
   * ensure there is enough room for the answer and truncate the question
   * to fit.
   */
  answer_buffer = mutt_buffer_pool_get ();
  mutt_buffer_printf (answer_buffer,
                      " ([%s]/%s%s): ",
                      def == MUTT_YES ? yes : no,
                      def == MUTT_YES ? no : yes,
                      show_help_prompt ? "/?" : "");
  answer_string_wid = mutt_strwidth (mutt_b2s (answer_buffer));

  msg_wid = mutt_strwidth (msg);

  FOREVER
  {
    if (redraw || SigWinch)
    {
      redraw = 0;
#if defined (USE_SLANG_CURSES) || defined (HAVE_RESIZETERM)
      if (SigWinch)
      {
        SigWinch = 0;
        mutt_resize_screen ();
        clearok (stdscr, TRUE);
        mutt_current_menu_redraw ();
      }
#endif
      if (MuttMessageWindow->cols)
      {
        prompt_lines = (msg_wid + answer_string_wid + MuttMessageWindow->cols - 1) /
          MuttMessageWindow->cols;
        prompt_lines = MAX (1, MIN (3, prompt_lines));
      }
      else
        prompt_lines = 1;

      /* maxlen here is sort of arbitrary, so pick a reasonable upper bound */
      trunc_msg_len = mutt_wstr_trunc (msg, 4 * prompt_lines * MuttMessageWindow->cols,
                                       prompt_lines * MuttMessageWindow->cols - answer_string_wid,
                                       NULL);

      if (show_help)
        prompt_lines += 1;

      if (prompt_lines != MuttMessageWindow->rows)
      {
        reflow_message_window_rows (prompt_lines);
        mutt_current_menu_redraw ();
      }

      mutt_window_move (MuttMessageWindow, 0, 0);
      SETCOLOR (MT_COLOR_PROMPT);
      if (show_help)
      {
        trunc_help_len = mutt_wstr_trunc (mutt_b2s (help_buffer),
                                          help_buffer->dsize,
                                          MuttMessageWindow->cols, NULL);
        addnstr (mutt_b2s (help_buffer), trunc_help_len);
        mutt_window_clrtoeol (MuttMessageWindow);
        mutt_window_move (MuttMessageWindow, 1, 0);
      }
      addnstr (msg, trunc_msg_len);
      addstr (mutt_b2s (answer_buffer));
      NORMAL_COLOR;
      mutt_window_clrtoeol (MuttMessageWindow);
    }

    mutt_refresh ();
    /* SigWinch is not processed unless timeout is set */
    mutt_getch_timeout (30 * 1000);
    ch = mutt_getch ();
    mutt_getch_timeout (-1);
    if (ch.ch == -2)
      continue;
    if (CI_is_return (ch.ch))
      break;
    if (ch.ch < 0)
    {
      def = -1;
      break;
    }

#ifdef HAVE_LANGINFO_YESEXPR
    answer[0] = ch.ch;
    if (reyes_ok ?
	(regexec (& reyes, answer, 0, 0, 0) == 0) :
#else
    if (
#endif
	(tolower (ch.ch) == 'y'))
    {
      def = MUTT_YES;
      break;
    }
    else if (
#ifdef HAVE_LANGINFO_YESEXPR
	     reno_ok ?
	     (regexec (& reno, answer, 0, 0, 0) == 0) :
#endif
	     (tolower (ch.ch) == 'n'))
    {
      def = MUTT_NO;
      break;
    }
    else if (show_help_prompt && ch.ch == '?')
    {
      show_help_prompt = 0;
      show_help = 1;
      redraw = 1;

      mutt_buffer_printf (answer_buffer,
                          " ([%s]/%s): ",
                          def == MUTT_YES ? yes : no,
                          def == MUTT_YES ? no : yes);
      answer_string_wid = mutt_strwidth (mutt_b2s (answer_buffer));

      help_buffer = mutt_buffer_pool_get ();
      /* L10N:
         In the mutt_yesorno() prompt, some variables and all
         quadoptions provide a '?' choice to provide the name of the
         configuration variable this prompt is dependent on.

         For example, the prompt "Quit Mutt?" is dependent on the
         quadoption $quit.

         Typing '?' at those prompts will print this message above
         the prompt, where %s is the name of the configuration
         variable.
      */
      mutt_buffer_printf (help_buffer, _("See $%s for more information."), var);
    }
    else
    {
      BEEP();
    }
  }

  mutt_buffer_pool_release (&answer_buffer);
  mutt_buffer_pool_release (&help_buffer);

#ifdef HAVE_LANGINFO_YESEXPR
  if (reyes_ok)
    regfree (& reyes);
  if (reno_ok)
    regfree (& reno);
#endif

  if (MuttMessageWindow->rows != 1)
  {
    reflow_message_window_rows (1);
    mutt_current_menu_redraw ();
  }
  else
    mutt_window_clearline (MuttMessageWindow, 0);

  if (def != -1)
  {
    mutt_window_mvaddstr (MuttMessageWindow, 0, 0,
                          def == MUTT_YES ? yes : no);
    mutt_refresh ();
  }
  else
  {
    /* when the users cancels with ^G, clear the message stored with
     * mutt_message() so it isn't displayed when the screen is refreshed. */
    mutt_clear_error();
  }
  return (def);
}

int mutt_yesorno (const char *msg, int def)
{
  return mutt_yesorno_with_help (msg, def, NULL);
}


/* this function is called when the user presses the abort key */
void mutt_query_exit (void)
{
  mutt_flushinp ();
  curs_set (1);
  if (Timeout)
    mutt_getch_timeout (-1); /* restore blocking operation */
  if (mutt_yesorno (_("Exit Mutt?"), MUTT_YES) == MUTT_YES)
  {
    if (!(mutt_background_has_backgrounded () &&
          option (OPTBACKGROUNDCONFIRMQUIT) &&
          mutt_query_boolean (OPTBACKGROUNDCONFIRMQUIT,
              _("There are $background_edit sessions. Really quit Mutt?"),
              MUTT_NO) != MUTT_YES))
    {
      endwin ();
      exit (1);
    }
  }
  mutt_clear_error();
  mutt_curs_set (-1);
  SigInt = 0;
}

void mutt_error_history_init (void)
{
  short i;

  if (OldErrorHistSize && ErrorHistory.msg)
  {
    for (i = 0; i < OldErrorHistSize; i++)
      FREE (&ErrorHistory.msg[i]);
    FREE (&ErrorHistory.msg);
  }

  if (ErrorHistSize)
    ErrorHistory.msg = safe_calloc (ErrorHistSize, sizeof (char *));
  ErrorHistory.last = 0;

  OldErrorHistSize = ErrorHistSize;
}

static void error_history_add (const char *s)
{
  static int in_process = 0;

  if (!ErrorHistSize || in_process || !s || !*s)
    return;
  in_process = 1;

  mutt_str_replace (&ErrorHistory.msg[ErrorHistory.last], s);
  if (++ErrorHistory.last >= ErrorHistSize)
    ErrorHistory.last = 0;

  in_process = 0;
}

static void error_history_dump (FILE *f)
{
  short cur;

  cur = ErrorHistory.last;
  do
  {
    if (ErrorHistory.msg[cur])
    {
      fputs (ErrorHistory.msg[cur], f);
      fputc ('\n', f);
    }
    if (++cur >= ErrorHistSize)
      cur = 0;
  } while (cur != ErrorHistory.last);
}

void mutt_error_history_display (void)
{
  static int in_process = 0;
  BUFFER *t = NULL;
  FILE *f;

  if (!ErrorHistSize)
  {
    mutt_error _("Error History is disabled.");
    return;
  }

  if (in_process)
  {
    mutt_error _("Error History is currently being shown.");
    return;
  }

  t = mutt_buffer_pool_get ();
  mutt_buffer_mktemp (t);
  if ((f = safe_fopen (mutt_b2s (t), "w")) == NULL)
  {
    mutt_perror (mutt_b2s (t));
    goto cleanup;
  }
  error_history_dump (f);
  safe_fclose (&f);

  in_process = 1;
  mutt_do_pager (_("Error History"), mutt_b2s (t), 0, NULL);
  in_process = 0;

cleanup:
  mutt_buffer_pool_release (&t);
}

static void curses_message (int error, const char *fmt, va_list ap)
{
  char scratch[LONG_STRING];

  vsnprintf (scratch, sizeof (scratch), fmt, ap);
  error_history_add (scratch);

  dprint (1, (debugfile, "%s\n", scratch));
  mutt_format_string (Errorbuf, sizeof (Errorbuf),
		      0, MuttMessageWindow->cols, FMT_LEFT, 0, scratch, sizeof (scratch), 0);

  if (!option (OPTKEEPQUIET))
  {
    if (error)
      BEEP ();
    SETCOLOR (error ? MT_COLOR_ERROR : MT_COLOR_MESSAGE);
    mutt_window_mvaddstr (MuttMessageWindow, 0, 0, Errorbuf);
    NORMAL_COLOR;
    mutt_window_clrtoeol (MuttMessageWindow);
    mutt_refresh ();
  }

  if (error)
    set_option (OPTMSGERR);
  else
    unset_option (OPTMSGERR);
}

void mutt_curses_error (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  curses_message (1, fmt, ap);
  va_end (ap);
}

void mutt_curses_message (const char *fmt, ...)
{
  va_list ap;

  va_start (ap, fmt);
  curses_message (0, fmt, ap);
  va_end (ap);
}

void mutt_progress_init (progress_t* progress, const char *msg,
			 unsigned short flags, unsigned short inc,
			 long size)
{
  struct timeval tv = { 0, 0 };

  if (!progress)
    return;
  if (option(OPTNOCURSES))
    return;

  memset (progress, 0, sizeof (progress_t));
  progress->inc = inc;
  progress->flags = flags;
  progress->msg = msg;
  progress->size = size;
  if (progress->size)
  {
    if (progress->flags & MUTT_PROGRESS_SIZE)
      mutt_pretty_size (progress->sizestr, sizeof (progress->sizestr),
			progress->size);
    else
      snprintf (progress->sizestr, sizeof (progress->sizestr), "%ld",
		progress->size);
  }
  if (!inc)
  {
    if (size)
      mutt_message ("%s (%s)", msg, progress->sizestr);
    else
      mutt_message (msg);
    return;
  }
  if (gettimeofday (&tv, NULL) < 0)
    dprint (1, (debugfile, "gettimeofday failed: %d\n", errno));
  /* if timestamp is 0 no time-based suppression is done */
  if (TimeInc)
    progress->timestamp_millis = ((unsigned long long) tv.tv_sec * 1000ULL)
      + (unsigned long long) (tv.tv_usec / 1000);
  mutt_progress_update (progress, 0, 0);
}

void mutt_progress_update (progress_t* progress, long pos, int percent)
{
  char posstr[SHORT_STRING];
  short update = 0;
  struct timeval tv = { 0, 0 };
  unsigned long long now_millis = 0;

  if (option(OPTNOCURSES))
    return;

  if (!progress->inc)
    goto out;

  /* refresh if size > inc */
  if (progress->flags & MUTT_PROGRESS_SIZE &&
      (pos >= progress->pos + (progress->inc << 10)))
    update = 1;
  else if (pos >= progress->pos + progress->inc)
    update = 1;

  /* skip refresh if not enough time has passed */
  if (update && progress->timestamp_millis && !gettimeofday (&tv, NULL))
  {
    now_millis = ((unsigned long long) tv.tv_sec * 1000ULL)
      + (unsigned long long) (tv.tv_usec / 1000);
    if (now_millis &&
        (now_millis - progress->timestamp_millis < TimeInc))
      update = 0;
  }

  /* always show the first update */
  if (!pos)
    update = 1;

  if (update)
  {
    if (progress->flags & MUTT_PROGRESS_SIZE)
    {
      pos = pos / (progress->inc << 10) * (progress->inc << 10);
      mutt_pretty_size (posstr, sizeof (posstr), pos);
    }
    else
      snprintf (posstr, sizeof (posstr), "%ld", pos);

    dprint (5, (debugfile, "updating progress: %s\n", posstr));

    progress->pos = pos;
    if (now_millis)
      progress->timestamp_millis = now_millis;

    if (progress->size > 0)
    {
      mutt_message ("%s %s/%s (%d%%)", progress->msg, posstr, progress->sizestr,
		    percent > 0 ? percent :
                    (int) (100.0 * (double) progress->pos / progress->size));
    }
    else
    {
      if (percent > 0)
	mutt_message ("%s %s (%d%%)", progress->msg, posstr, percent);
      else
	mutt_message ("%s %s", progress->msg, posstr);
    }
  }

out:
  if (pos >= progress->size)
    mutt_clear_error ();
}

void mutt_init_windows (void)
{
  MuttHelpWindow = safe_calloc (sizeof (mutt_window_t), 1);
  MuttIndexWindow = safe_calloc (sizeof (mutt_window_t), 1);
  MuttStatusWindow = safe_calloc (sizeof (mutt_window_t), 1);
  MuttMessageWindow = safe_calloc (sizeof (mutt_window_t), 1);
#ifdef USE_SIDEBAR
  MuttSidebarWindow = safe_calloc (sizeof (mutt_window_t), 1);
#endif
}

void mutt_free_windows (void)
{
  FREE (&MuttHelpWindow);
  FREE (&MuttIndexWindow);
  FREE (&MuttStatusWindow);
  FREE (&MuttMessageWindow);
#ifdef USE_SIDEBAR
  FREE (&MuttSidebarWindow);
#endif
}

void mutt_reflow_windows (void)
{
  if (option (OPTNOCURSES))
    return;

  dprint (2, (debugfile, "In mutt_reflow_windows\n"));

  MuttStatusWindow->rows = 1;
  MuttStatusWindow->cols = COLS;
  MuttStatusWindow->row_offset = option (OPTSTATUSONTOP) ? 0 : LINES - 2;
  MuttStatusWindow->col_offset = 0;

  memcpy (MuttHelpWindow, MuttStatusWindow, sizeof (mutt_window_t));
  if (! option (OPTHELP))
    MuttHelpWindow->rows = 0;
  else
    MuttHelpWindow->row_offset = option (OPTSTATUSONTOP) ? LINES - 2 : 0;

  memcpy (MuttMessageWindow, MuttStatusWindow, sizeof (mutt_window_t));
  MuttMessageWindow->row_offset = LINES - 1;

  memcpy (MuttIndexWindow, MuttStatusWindow, sizeof (mutt_window_t));
  MuttIndexWindow->rows = MAX(LINES - MuttStatusWindow->rows -
			      MuttHelpWindow->rows - MuttMessageWindow->rows, 0);
  MuttIndexWindow->row_offset = option (OPTSTATUSONTOP) ? MuttStatusWindow->rows :
                                                          MuttHelpWindow->rows;

#ifdef USE_SIDEBAR
  if (option (OPTSIDEBAR))
  {
    memcpy (MuttSidebarWindow, MuttIndexWindow, sizeof (mutt_window_t));
    MuttSidebarWindow->cols = MAX (SidebarWidth, 0);
    /* Ensure the index window has at least one column, to prevent
     * pager regressions. */
    if (MuttSidebarWindow->cols >= MuttIndexWindow->cols)
      MuttSidebarWindow->cols = MuttIndexWindow->cols - 1;

    MuttIndexWindow->cols -= MuttSidebarWindow->cols;
    MuttIndexWindow->col_offset += MuttSidebarWindow->cols;
  }
#endif

  mutt_set_current_menu_redraw_full ();
  /* the pager menu needs this flag set to recalc lineInfo */
  mutt_set_current_menu_redraw (REDRAW_FLOW);
}

static void reflow_message_window_rows (int mw_rows)
{
  MuttMessageWindow->rows = mw_rows;
  MuttMessageWindow->row_offset = LINES - mw_rows;

  MuttStatusWindow->row_offset = option (OPTSTATUSONTOP) ? 0 : LINES - mw_rows - 1;

  if (option (OPTHELP))
    MuttHelpWindow->row_offset = option (OPTSTATUSONTOP) ? LINES - mw_rows - 1 : 0;

  MuttIndexWindow->rows = MAX(LINES - MuttStatusWindow->rows -
			      MuttHelpWindow->rows - MuttMessageWindow->rows, 0);

#ifdef USE_SIDEBAR
  if (option (OPTSIDEBAR))
    MuttSidebarWindow->rows = MuttIndexWindow->rows;
#endif

  /* We don't also set REDRAW_FLOW because this function only
   * changes rows and is a temporary adjustment. */
  mutt_set_current_menu_redraw_full ();
}

int mutt_window_move (mutt_window_t *win, int row, int col)
{
  return move (win->row_offset + row, win->col_offset + col);
}

int mutt_window_mvaddch (mutt_window_t *win, int row, int col, const chtype ch)
{
  return mvaddch (win->row_offset + row, win->col_offset + col, ch);
}

int mutt_window_mvaddstr (mutt_window_t *win, int row, int col, const char *str)
{
  return mvaddstr (win->row_offset + row, win->col_offset + col, str);
}

#ifdef USE_SLANG_CURSES
static int vw_printw (SLcurses_Window_Type *win, const char *fmt, va_list ap)
{
  char buf[LONG_STRING];

  (void) SLvsnprintf (buf, sizeof (buf), (char *)fmt, ap);
  SLcurses_waddnstr (win, buf, -1);
  return 0;
}
#endif

int mutt_window_mvprintw (mutt_window_t *win, int row, int col, const char *fmt, ...)
{
  va_list ap;
  int rv;

  if ((rv = mutt_window_move (win, row, col)) != ERR)
  {
    va_start (ap, fmt);
    rv = vw_printw (stdscr, fmt, ap);
    va_end (ap);
  }

  return rv;
}

/* Assumes the cursor has already been positioned within the
 * window.
 */
void mutt_window_clrtoeol (mutt_window_t *win)
{
  int row, col, curcol;

  if (win->col_offset + win->cols == COLS)
    clrtoeol ();
  else
  {
    getyx (stdscr, row, col);
    curcol = col;
    while (curcol < win->col_offset + win->cols)
    {
      addch (' ');
      curcol++;
    }
    move (row, col);
  }
}

void mutt_window_clearline (mutt_window_t *win, int row)
{
  mutt_window_move (win, row, 0);
  mutt_window_clrtoeol (win);
}

/* Assumes the current position is inside the window.
 * Otherwise it will happily return negative or values outside
 * the window boundaries
 */
void mutt_window_getyx (mutt_window_t *win, int *y, int *x)
{
  int row, col;

  getyx (stdscr, row, col);
  if (y)
    *y = row - win->row_offset;
  if (x)
    *x = col - win->col_offset;
}


void mutt_show_error (void)
{
  if (option (OPTKEEPQUIET))
    return;

  SETCOLOR (option (OPTMSGERR) ? MT_COLOR_ERROR : MT_COLOR_MESSAGE);
  mutt_window_mvaddstr (MuttMessageWindow, 0, 0, Errorbuf);
  NORMAL_COLOR;
  mutt_window_clrtoeol(MuttMessageWindow);
}

void mutt_endwin (const char *msg)
{
  int e = errno;

  if (!option (OPTNOCURSES))
  {
    /* at least in some situations (screen + xterm under SuSE11/12) endwin()
     * doesn't properly flush the screen without an explicit call.
     */
    mutt_refresh();
    endwin ();
    SigWinch = 1;
  }

  if (msg && *msg)
  {
    puts (msg);
    fflush (stdout);
  }

  errno = e;
}

void mutt_perror (const char *s)
{
  const char *p = strerror (errno);

  dprint (1, (debugfile, "%s: %s (errno = %d)\n", s,
              p ? p : "unknown error", errno));
  mutt_error ("%s: %s (errno = %d)", s, p ? p : _("unknown error"), errno);
}

int mutt_any_key_to_continue (const char *s)
{
  struct termios t;
  struct termios old;
  int f, ch;

  f = open ("/dev/tty", O_RDONLY);
  tcgetattr (f, &t);
  memcpy ((void *)&old, (void *)&t, sizeof(struct termios)); /* save original state */
  t.c_lflag &= ~(ICANON | ECHO);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr (f, TCSADRAIN, &t);
  fflush (stdout);
  if (s)
    fputs (s, stdout);
  else
    fputs (_("Press any key to continue..."), stdout);
  fflush (stdout);
  ch = fgetc (stdin);
  fflush (stdin);
  tcsetattr (f, TCSADRAIN, &old);
  close (f);
  fputs ("\r\n", stdout);
  mutt_clear_error ();
  return (ch);
}

int mutt_do_pager (const char *banner,
		   const char *tempfile,
		   int do_color,
		   pager_t *info)
{
  int rc;

  if (!Pager || mutt_strcmp (Pager, "builtin") == 0)
    rc = mutt_pager (banner, tempfile, do_color, info);
  else
  {
    BUFFER *cmd = NULL;

    cmd = mutt_buffer_pool_get ();
    mutt_endwin (NULL);
    mutt_expand_file_fmt (cmd, Pager, tempfile);
    if (mutt_system (mutt_b2s (cmd)) == -1)
    {
      mutt_error (_("Error running \"%s\"!"), mutt_b2s (cmd));
      rc = -1;
    }
    else
      rc = 0;
    mutt_unlink (tempfile);
    mutt_buffer_pool_release (&cmd);
  }

  return rc;
}

static int _enter_fname (const char *prompt, BUFFER *fname, int flags,
                        int multiple, char ***files, int *numfiles)
{
  event_t ch;

  SETCOLOR (MT_COLOR_PROMPT);
  mutt_window_mvaddstr (MuttMessageWindow, 0, 0, (char *) prompt);
  addstr (_(" ('?' for list): "));
  NORMAL_COLOR;
  if (mutt_buffer_len (fname))
    addstr (mutt_b2s (fname));
  mutt_window_clrtoeol (MuttMessageWindow);
  mutt_refresh ();

  do
  {
    ch = mutt_getch();
  } while (ch.ch == -2);
  if (ch.ch < 0)
  {
    mutt_window_clearline (MuttMessageWindow, 0);
    return (-1);
  }
  else if (ch.ch == '?')
  {
    mutt_refresh ();
    mutt_buffer_clear (fname);
    _mutt_buffer_select_file (fname,
                              MUTT_SEL_FOLDER | (multiple ? MUTT_SEL_MULTI : 0),
                              files, numfiles);
  }
  else
  {
    char *pc = safe_malloc (mutt_strlen (prompt) + 3);

    sprintf (pc, "%s: ", prompt);	/* __SPRINTF_CHECKED__ */
    mutt_unget_event (ch.op ? 0 : ch.ch, ch.op ? ch.op : 0);

    mutt_buffer_increase_size (fname, LONG_STRING);
    if (_get_field (pc, fname,
                    flags | MUTT_CLEAR,
                    multiple, files, numfiles) != 0)
      mutt_buffer_clear (fname);
    FREE (&pc);
  }

  return 0;
}

int mutt_enter_mailbox (const char *prompt, BUFFER *fname, int do_incoming)
{
  int flags = MUTT_MAILBOX;

  if (do_incoming)
    flags |= MUTT_INCOMING;

  return _enter_fname (prompt, fname, flags, 0, NULL, NULL);
}

int mutt_enter_filename (const char *prompt, BUFFER *fname)
{
  return _enter_fname (prompt, fname, MUTT_FILE, 0, NULL, NULL);
}

int mutt_enter_filenames (const char *prompt, char ***files, int *numfiles)
{
  BUFFER *tmp;
  int rc;

  tmp = mutt_buffer_pool_get ();
  rc = _enter_fname (prompt, tmp, MUTT_FILE, 1, files, numfiles);
  mutt_buffer_pool_release (&tmp);
  return rc;
}

void mutt_unget_event (int ch, int op)
{
  event_t tmp;

  tmp.ch = ch;
  tmp.op = op;

  if (UngetCount >= UngetLen)
    safe_realloc (&UngetKeyEvents, (UngetLen += 16) * sizeof(event_t));

  UngetKeyEvents[UngetCount++] = tmp;
}

void mutt_unget_string (char *s)
{
  char *p = s + mutt_strlen (s) - 1;

  while (p >= s)
  {
    mutt_unget_event ((unsigned char)*p--, 0);
  }
}

/*
 * Adds the ch/op to the macro buffer.
 * This should be used for macros, push, and exec commands only.
 */
void mutt_push_macro_event (int ch, int op)
{
  event_t tmp;

  tmp.ch = ch;
  tmp.op = op;

  if (MacroBufferCount >= MacroBufferLen)
    safe_realloc (&MacroEvents, (MacroBufferLen += 128) * sizeof(event_t));

  MacroEvents[MacroBufferCount++] = tmp;
}

void mutt_flush_macro_to_endcond (void)
{
  UngetCount = 0;
  while (MacroBufferCount > 0)
  {
    if (MacroEvents[--MacroBufferCount].op == OP_END_COND)
      return;
  }
}

/* Normally, OP_END_COND should only be in the MacroEvent buffer.
 * km_error_key() (ab)uses OP_END_COND as a barrier in the unget
 * buffer, and calls this function to flush. */
void mutt_flush_unget_to_endcond (void)
{
  while (UngetCount > 0)
  {
    if (UngetKeyEvents[--UngetCount].op == OP_END_COND)
      return;
  }
}

void mutt_flushinp (void)
{
  UngetCount = 0;
  MacroBufferCount = 0;
  flushinp ();
}

#if (defined(USE_SLANG_CURSES) || defined(HAVE_CURS_SET))
/* The argument can take 3 values:
 * -1: restore the value of the last call
 *  0: make the cursor invisible
 *  1: make the cursor visible
 */
void mutt_curs_set (int cursor)
{
  static int SavedCursor = 1;

  if (cursor < 0)
    cursor = SavedCursor;
  else
    SavedCursor = cursor;

  if (curs_set (cursor) == ERR)
  {
    if (cursor == 1)	/* cnorm */
      curs_set (2);	/* cvvis */
  }
}
#endif

int mutt_multi_choice (char *prompt, char *letters)
{
  event_t ch;
  int choice;
  int redraw = 1, prompt_lines = 1;
  char *p;

  FOREVER
  {
    if (redraw || SigWinch)
    {
      redraw = 0;
#if defined (USE_SLANG_CURSES) || defined (HAVE_RESIZETERM)
      if (SigWinch)
      {
        SigWinch = 0;
        mutt_resize_screen ();
        clearok (stdscr, TRUE);
        mutt_current_menu_redraw ();
      }
#endif
      if (MuttMessageWindow->cols)
      {
        prompt_lines = (mutt_strwidth (prompt) + MuttMessageWindow->cols - 1) /
          MuttMessageWindow->cols;
        prompt_lines = MAX (1, MIN (3, prompt_lines));
      }
      if (prompt_lines != MuttMessageWindow->rows)
      {
        reflow_message_window_rows (prompt_lines);
        mutt_current_menu_redraw ();
      }

      SETCOLOR (MT_COLOR_PROMPT);
      mutt_window_mvaddstr (MuttMessageWindow, 0, 0, prompt);
      NORMAL_COLOR;
      mutt_window_clrtoeol (MuttMessageWindow);
    }

    mutt_refresh ();
    /* SigWinch is not processed unless timeout is set */
    mutt_getch_timeout (30 * 1000);
    ch  = mutt_getch ();
    mutt_getch_timeout (-1);
    if (ch.ch == -2)
      continue;
    /* (ch.ch == 0) is technically possible.  Treat the same as < 0 (abort) */
    if (ch.ch <= 0 || CI_is_return (ch.ch))
    {
      choice = -1;
      break;
    }
    else
    {
      p = strchr (letters, ch.ch);
      if (p)
      {
	choice = p - letters + 1;
	break;
      }
      else if (ch.ch <= '9' && ch.ch > '0')
      {
	choice = ch.ch - '0';
	if (choice <= mutt_strlen (letters))
	  break;
      }
    }
    BEEP ();
  }
  if (MuttMessageWindow->rows != 1)
  {
    reflow_message_window_rows (1);
    mutt_current_menu_redraw ();
  }
  else
    mutt_window_clearline (MuttMessageWindow, 0);
  mutt_refresh ();
  return choice;
}

/*
 * addwch would be provided by an up-to-date curses library
 */

int mutt_addwch (wchar_t wc)
{
  char buf[MB_LEN_MAX*2];
  mbstate_t mbstate;
  size_t n1, n2;

  memset (&mbstate, 0, sizeof (mbstate));
  if ((n1 = wcrtomb (buf, wc, &mbstate)) == (size_t)(-1) ||
      (n2 = wcrtomb (buf + n1, 0, &mbstate)) == (size_t)(-1))
    return -1; /* ERR */
  else
    return addstr (buf);
}


/*
 * This formats a string, a bit like
 * snprintf (dest, destlen, "%-*.*s", min_width, max_width, s),
 * except that the widths refer to the number of character cells
 * when printed.
 */

void mutt_format_string (char *dest, size_t destlen,
			 int min_width, int max_width,
			 int justify, char m_pad_char,
			 const char *s, size_t n,
			 int arboreal)
{
  char *p;
  wchar_t wc;
  int w;
  size_t k, k2;
  char scratch[MB_LEN_MAX];
  mbstate_t mbstate1, mbstate2;

  memset(&mbstate1, 0, sizeof (mbstate1));
  memset(&mbstate2, 0, sizeof (mbstate2));
  --destlen;
  p = dest;
  for (; n && (k = mbrtowc (&wc, s, n, &mbstate1)); s += k, n -= k)
  {
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      if (k == (size_t)(-1) && errno == EILSEQ)
	memset (&mbstate1, 0, sizeof (mbstate1));

      k = (k == (size_t)(-1)) ? 1 : n;
      wc = replacement_char ();
    }
    if (arboreal && wc < MUTT_TREE_MAX)
      w = 1; /* hack */
    else
    {
#ifdef HAVE_ISWBLANK
      if (iswblank (wc))
	wc = ' ';
      else
#endif
        if (!IsWPrint (wc))
          wc = '?';
      w = wcwidth (wc);
    }
    if (w >= 0)
    {
      if (w > max_width || (k2 = wcrtomb (scratch, wc, &mbstate2)) > destlen)
	break;
      min_width -= w;
      max_width -= w;
      strncpy (p, scratch, k2);
      p += k2;
      destlen -= k2;
    }
  }
  w = (int)destlen < min_width ? destlen : min_width;
  if (w <= 0)
    *p = '\0';
  else if (justify == FMT_RIGHT)	/* right justify */
  {
    p[w] = '\0';
    while (--p >= dest)
      p[w] = *p;
    while (--w >= 0)
      dest[w] = m_pad_char;
  }
  else if (justify == FMT_CENTER)	/* center */
  {
    char *savedp = p;
    int half = (w+1) / 2; /* half of cushion space */

    p[w] = '\0';

    /* move str to center of buffer */
    while (--p >= dest)
      p[half] = *p;

    /* fill rhs */
    p = savedp + half;
    while (--w >= half)
      *p++ = m_pad_char;

    /* fill lhs */
    while (half--)
      dest[half] = m_pad_char;
  }
  else					/* left justify */
  {
    while (--w >= 0)
      *p++ = m_pad_char;
    *p = '\0';
  }
}

/*
 * This formats a string rather like
 *   snprintf (fmt, sizeof (fmt), "%%%ss", prefix);
 *   snprintf (dest, destlen, fmt, s);
 * except that the numbers in the conversion specification refer to
 * the number of character cells when printed.
 */

static void mutt_format_s_x (char *dest,
			     size_t destlen,
			     const char *prefix,
			     const char *s,
			     int arboreal)
{
  int justify = FMT_RIGHT;
  char *p;
  int min_width;
  int max_width = INT_MAX;

  if (*prefix == '-')
    ++prefix, justify = FMT_LEFT;
  else if (*prefix == '=')
    ++prefix, justify = FMT_CENTER;
  min_width = strtol (prefix, &p, 10);
  if (*p == '.')
  {
    prefix = p + 1;
    max_width = strtol (prefix, &p, 10);
    if (p <= prefix)
      max_width = INT_MAX;
  }

  mutt_format_string (dest, destlen, min_width, max_width,
		      justify, ' ', s, mutt_strlen (s), arboreal);
}

void mutt_format_s (char *dest,
		    size_t destlen,
		    const char *prefix,
		    const char *s)
{
  mutt_format_s_x (dest, destlen, prefix, s, 0);
}

void mutt_format_s_tree (char *dest,
			 size_t destlen,
			 const char *prefix,
			 const char *s)
{
  mutt_format_s_x (dest, destlen, prefix, s, 1);
}

/*
 * mutt_paddstr (n, s) is almost equivalent to
 * mutt_format_string (bigbuf, big, n, n, FMT_LEFT, ' ', s, big, 0), addstr (bigbuf)
 */

void mutt_paddstr (int n, const char *s)
{
  wchar_t wc;
  int w;
  size_t k;
  size_t len = mutt_strlen (s);
  mbstate_t mbstate;

  memset (&mbstate, 0, sizeof (mbstate));
  for (; len && (k = mbrtowc (&wc, s, len, &mbstate)); s += k, len -= k)
  {
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      if (k == (size_t) (-1))
	memset (&mbstate, 0, sizeof (mbstate));
      k = (k == (size_t)(-1)) ? 1 : len;
      wc = replacement_char ();
    }
    if (!IsWPrint (wc))
      wc = '?';
    w = wcwidth (wc);
    if (w >= 0)
    {
      if (w > n)
	break;
      mutt_addwch (wc);
      n -= w;
    }
  }
  while (n-- > 0)
    addch (' ');
}

/* See how many bytes to copy from string so its at most maxlen bytes
 * long and maxwid columns wide */
size_t mutt_wstr_trunc (const char *src, size_t maxlen, size_t maxwid, size_t *width)
{
  wchar_t wc;
  size_t n, w = 0, l = 0, cl;
  int cw;
  mbstate_t mbstate;

  if (!src)
    goto out;

  n = mutt_strlen (src);

  memset (&mbstate, 0, sizeof (mbstate));
  for (w = 0; n && (cl = mbrtowc (&wc, src, n, &mbstate)); src += cl, n -= cl)
  {
    if (cl == (size_t)(-1) || cl == (size_t)(-2))
    {
      if (cl == (size_t)(-1))
        memset (&mbstate, 0, sizeof (mbstate));
      cl = (cl == (size_t)(-1)) ? 1 : n;
      wc = replacement_char ();
    }
    cw = wcwidth (wc);
    /* hack because MUTT_TREE symbols aren't turned into characters
     * until rendered by print_enriched_string (#3364) */
    if (cw < 0 && cl == 1 && src[0] && src[0] < MUTT_TREE_MAX)
      cw = 1;
    else if (cw < 0)
      cw = 0;			/* unprintable wchar */
    if (cl + l > maxlen || cw + w > maxwid)
      break;
    l += cl;
    w += cw;
  }
out:
  if (width)
    *width = w;
  return l;
}

/*
 * returns the number of bytes the first (multibyte) character
 * of input consumes:
 * 	< 0 ... conversion error
 * 	= 0 ... end of input
 * 	> 0 ... length (bytes)
 */
int mutt_charlen (const char *s, int *width)
{
  wchar_t wc;
  mbstate_t mbstate;
  size_t k, n;

  if (!s || !*s)
    return 0;

  n = mutt_strlen (s);
  memset (&mbstate, 0, sizeof (mbstate));
  k = mbrtowc (&wc, s, n, &mbstate);
  if (width)
    *width = wcwidth (wc);
  return (k == (size_t)(-1) || k == (size_t)(-2)) ? -1 : k;
}

/*
 * mutt_strwidth is like mutt_strlen except that it returns the width
 * referring to the number of character cells.
 */

int mutt_strwidth (const char *s)
{
  wchar_t wc;
  int w;
  size_t k, n;
  mbstate_t mbstate;

  if (!s) return 0;

  n = mutt_strlen (s);

  memset (&mbstate, 0, sizeof (mbstate));
  for (w=0; n && (k = mbrtowc (&wc, s, n, &mbstate)); s += k, n -= k)
  {
    if (k == (size_t)(-1) || k == (size_t)(-2))
    {
      if (k == (size_t)(-1))
        memset (&mbstate, 0, sizeof (mbstate));
      k = (k == (size_t)(-1)) ? 1 : n;
      wc = replacement_char ();
    }
    if (!IsWPrint (wc))
      wc = '?';
    w += wcwidth (wc);
  }
  return w;
}
