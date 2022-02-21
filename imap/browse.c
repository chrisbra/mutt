/*
 * Copyright (C) 1996-1999 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999-2008 Brendan Cully <brendan@kublai.com>
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

/* Mutt browser support routines */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include <stdlib.h>
#include <ctype.h>

#include "mutt.h"
#include "buffy.h"
#include "imap_private.h"

/* -- forward declarations -- */
static int browse_add_list_result (IMAP_DATA* idata, const char* cmd,
                                   struct browser_state* state, short isparent);
static void imap_add_folder (char delim, char *folder, int noselect,
                             int noinferiors, struct browser_state *state, short isparent);
static int compare_names(struct folder_file *a, struct folder_file *b);

/* imap_browse: IMAP hook into the folder browser, fills out browser_state,
 *   given a current folder to browse */
int imap_browse (const char* path, struct browser_state* state)
{
  IMAP_DATA* idata;
  IMAP_LIST list;
  char buf[LONG_STRING*2];
  char mbox[LONG_STRING];
  char munged_mbox[LONG_STRING];
  const char *list_cmd;
  int len;
  int n;
  int nsup;
  char ctmp;
  short showparents = 0;
  int save_lsub;
  IMAP_MBOX mx;

  if (imap_parse_path (path, &mx))
  {
    mutt_error (_("%s is an invalid IMAP path"), path);
    return -1;
  }

  save_lsub = option (OPTIMAPCHECKSUBSCRIBED);
  unset_option (OPTIMAPCHECKSUBSCRIBED);

  if (!(idata = imap_conn_find (&(mx.account), 0)))
    goto fail;

  if (option (OPTIMAPLSUB))
  {
    /* RFC3348 section 3 states LSUB is unreliable for hierarchy information.
     * The newer LIST extensions are designed for this.
     */
    if (mutt_bit_isset (idata->capabilities, LIST_EXTENDED))
      list_cmd = "LIST (SUBSCRIBED RECURSIVEMATCH)";
    else
      list_cmd = "LSUB";
  }
  else
  {
    list_cmd = "LIST";
  }

  mutt_message _("Getting folder list...");

  /* skip check for parents when at the root */
  if (mx.mbox && mx.mbox[0] != '\0')
  {
    imap_fix_path (idata, mx.mbox, mbox, sizeof (mbox));
    n = mutt_strlen (mbox);
  }
  else
  {
    mbox[0] = '\0';
    n = 0;
  }

  if (n)
  {
    int rc;
    dprint (3, (debugfile, "imap_browse: mbox: %s\n", mbox));

    /* if our target exists and has inferiors, enter it if we
     * aren't already going to */
    imap_munge_mbox_name (idata, munged_mbox, sizeof (munged_mbox), mbox);
    len = snprintf (buf, sizeof (buf), "%s \"\" %s", list_cmd, munged_mbox);
    if (mutt_bit_isset (idata->capabilities, LIST_EXTENDED))
      snprintf (buf + len, sizeof(buf) - len, " RETURN (CHILDREN)");
    imap_cmd_start (idata, buf);
    idata->cmdtype = IMAP_CT_LIST;
    idata->cmddata = &list;
    do
    {
      list.name = 0;
      rc = imap_cmd_step (idata);
      if (rc == IMAP_CMD_CONTINUE && list.name)
      {
        if (!list.noinferiors && list.name[0] &&
            !imap_mxcmp (list.name, mbox) &&
            n < sizeof (mbox) - 1)
        {
          mbox[n++] = list.delim;
          mbox[n] = '\0';
        }
      }
    }
    while (rc == IMAP_CMD_CONTINUE);
    idata->cmddata = NULL;

    /* if we're descending a folder, mark it as current in browser_state */
    if (mbox[n-1] == list.delim)
    {
      showparents = 1;
      imap_qualify_path (buf, sizeof (buf), &mx, mbox);
      state->folder = safe_strdup (buf);
      n--;
    }

    /* Find superiors to list
     * Note: UW-IMAP servers return folder + delimiter when asked to list
     *  folder + delimiter. Cyrus servers don't. So we ask for folder,
     *  and tack on delimiter ourselves.
     * Further note: UW-IMAP servers return nothing when asked for
     *  NAMESPACES without delimiters at the end. Argh! */
    for (n--; n >= 0 && mbox[n] != list.delim ; n--);
    if (n > 0)			/* "aaaa/bbbb/" -> "aaaa" */
    {
      /* forget the check, it is too delicate (see above). Have we ever
       * had the parent not exist? */
      ctmp = mbox[n];
      mbox[n] = '\0';

      if (showparents)
      {
	dprint (3, (debugfile, "imap_init_browse: adding parent %s\n", mbox));
	imap_add_folder (list.delim, mbox, 1, 0, state, 1);
      }

      /* if our target isn't a folder, we are in our superior */
      if (!state->folder)
      {
        /* store folder with delimiter */
        mbox[n++] = ctmp;
        ctmp = mbox[n];
        mbox[n] = '\0';
        imap_qualify_path (buf, sizeof (buf), &mx, mbox);
        state->folder = safe_strdup (buf);
      }
      mbox[n] = ctmp;
    }
    /* "/bbbb/" -> add  "/", "aaaa/" -> add "" */
    else
    {
      char relpath[2];
      /* folder may be "/" */
      snprintf (relpath, sizeof (relpath), "%c" , n < 0 ? '\0' : idata->delim);
      if (showparents)
        imap_add_folder (idata->delim, relpath, 1, 0, state, 1);
      if (!state->folder)
      {
        imap_qualify_path (buf, sizeof (buf), &mx, relpath);
        state->folder = safe_strdup (buf);
      }
    }
  }

  /* no namespace, no folder: set folder to host only */
  if (!state->folder)
  {
    imap_qualify_path (buf, sizeof (buf), &mx, NULL);
    state->folder = safe_strdup (buf);
  }

  nsup = state->entrylen;

  dprint (3, (debugfile, "imap_browse: Quoting mailbox scan: %s -> ", mbox));
  snprintf (buf, sizeof (buf), "%s%%", mbox);
  imap_munge_mbox_name (idata, munged_mbox, sizeof (munged_mbox), buf);
  dprint (3, (debugfile, "%s\n", munged_mbox));
  len = snprintf (buf, sizeof (buf), "%s \"\" %s", list_cmd, munged_mbox);
  if (mutt_bit_isset (idata->capabilities, LIST_EXTENDED))
    snprintf (buf + len, sizeof(buf) - len, " RETURN (CHILDREN)");
  if (browse_add_list_result (idata, buf, state, 0))
    goto fail;

  if (!state->entrylen)
  {
    mutt_error _("No such folder");
    goto fail;
  }

  mutt_clear_error ();

  qsort(&(state->entry[nsup]),state->entrylen-nsup,sizeof(state->entry[0]),
	(int (*)(const void*,const void*)) compare_names);

  if (save_lsub)
    set_option (OPTIMAPCHECKSUBSCRIBED);

  FREE (&mx.mbox);
  return 0;

fail:
  if (save_lsub)
    set_option (OPTIMAPCHECKSUBSCRIBED);
  FREE (&mx.mbox);
  return -1;
}

/* imap_mailbox_create: Prompt for a new mailbox name, and try to create it */
int imap_mailbox_create (const char* folder, BUFFER *result)
{
  IMAP_DATA* idata;
  IMAP_MBOX mx;
  char buf[LONG_STRING];
  short n;

  if (imap_parse_path (folder, &mx) < 0)
  {
    dprint (1, (debugfile, "imap_mailbox_create: Bad starting path %s\n",
                folder));
    return -1;
  }

  if (!(idata = imap_conn_find (&mx.account, MUTT_IMAP_CONN_NONEW)))
  {
    dprint (1, (debugfile, "imap_mailbox_create: Couldn't find open connection to %s", mx.account.host));
    goto fail;
  }

  strfcpy (buf, NONULL (mx.mbox), sizeof (buf));

  /* append a delimiter if necessary */
  n = mutt_strlen (buf);
  if (n && (n < sizeof (buf) - 1) && (buf[n-1] != idata->delim))
  {
    buf[n++] = idata->delim;
    buf[n] = '\0';
  }

  if (mutt_get_field (_("Create mailbox: "), buf, sizeof (buf), MUTT_MAILBOX) < 0)
    goto fail;

  if (!mutt_strlen (buf))
  {
    mutt_error (_("Mailbox must have a name."));
    mutt_sleep(1);
    goto fail;
  }

  if (imap_create_mailbox (idata, buf) < 0)
    goto fail;

  imap_buffer_qualify_path (result, &mx, buf);

  mutt_message _("Mailbox created.");
  mutt_sleep (0);

  FREE (&mx.mbox);
  return 0;

fail:
  FREE (&mx.mbox);
  return -1;
}

int imap_mailbox_rename(const char* mailbox, BUFFER *result)
{
  IMAP_DATA* idata;
  IMAP_MBOX mx;
  char buf[LONG_STRING];
  char newname[SHORT_STRING];

  if (imap_parse_path (mailbox, &mx) < 0)
  {
    dprint (1, (debugfile, "imap_mailbox_rename: Bad source mailbox %s\n",
                mailbox));
    return -1;
  }

  if (!(idata = imap_conn_find (&mx.account, MUTT_IMAP_CONN_NONEW)))
  {
    dprint (1, (debugfile, "imap_mailbox_rename: Couldn't find open connection to %s", mx.account.host));
    goto fail;
  }

  if (!mx.mbox)
  {
    mutt_error _("Cannot rename root folder");
    goto fail;
  }

  snprintf(buf, sizeof (buf), _("Rename mailbox %s to: "), mx.mbox);
  strfcpy (newname, mx.mbox, sizeof (newname));

  if (mutt_get_field (buf, newname, sizeof (newname), MUTT_MAILBOX) < 0)
    goto fail;

  if (!mutt_strlen (newname))
  {
    mutt_error (_("Mailbox must have a name."));
    mutt_sleep (1);
    goto fail;
  }

  imap_fix_path (idata, newname, buf, sizeof (buf));

  if (imap_rename_mailbox (idata, &mx, buf) < 0)
  {
    mutt_error (_("Rename failed: %s"), imap_get_qualifier (idata->buf));
    mutt_sleep (1);
    goto fail;
  }

  imap_buffer_qualify_path (result, &mx, buf);

  mutt_message (_("Mailbox renamed."));
  mutt_sleep (0);

  FREE (&mx.mbox);
  return 0;

fail:
  FREE (&mx.mbox);
  return -1;
}

static int browse_add_list_result (IMAP_DATA* idata, const char* cmd,
                                   struct browser_state* state, short isparent)
{
  IMAP_LIST list;
  IMAP_MBOX mx;
  int rc;

  if (imap_parse_path (state->folder, &mx))
  {
    dprint (2, (debugfile,
                "browse_add_list_result: current folder %s makes no sense\n", state->folder));
    return -1;
  }

  imap_cmd_start (idata, cmd);
  idata->cmdtype = IMAP_CT_LIST;
  idata->cmddata = &list;
  do
  {
    list.name = NULL;
    rc = imap_cmd_step (idata);

    if (rc == IMAP_CMD_CONTINUE && list.name)
    {
      /* Let a parent folder never be selectable for navigation */
      if (isparent)
        list.noselect = 1;
      /* prune current folder from output */
      if (isparent || mutt_strncmp (list.name, mx.mbox, strlen (list.name)))
        imap_add_folder (list.delim, list.name, list.noselect, list.noinferiors,
                         state, isparent);
    }
  }
  while (rc == IMAP_CMD_CONTINUE);
  idata->cmddata = NULL;

  FREE (&mx.mbox);
  return rc == IMAP_CMD_OK ? 0 : -1;
}

/* imap_add_folder:
 * add a folder name to the browser list, formatting it as necessary.
 *
 * The folder parameter should already be 'unmunged' via
 * imap_unmunge_mbox_name().
 */
static void imap_add_folder (char delim, char *folder, int noselect,
                             int noinferiors, struct browser_state *state, short isparent)
{
  char tmp[LONG_STRING];
  char relpath[LONG_STRING];
  IMAP_MBOX mx;
  BUFFY *b;

  if (imap_parse_path (state->folder, &mx))
    return;

  if (state->entrylen == state->entrymax)
  {
    safe_realloc (&state->entry,
                  sizeof (struct folder_file) * (state->entrymax += 256));
    memset (state->entry + state->entrylen, 0,
            (sizeof (struct folder_file) * (state->entrymax - state->entrylen)));
  }

  /* render superiors as unix-standard ".." */
  if (isparent)
    strfcpy (relpath, "../", sizeof (relpath));
  /* strip current folder from target, to render a relative path */
  else if (!mutt_strncmp (mx.mbox, folder, mutt_strlen (mx.mbox)))
    strfcpy (relpath, folder + mutt_strlen (mx.mbox), sizeof (relpath));
  else
    strfcpy (relpath, folder, sizeof (relpath));

  /* apply filemask filter. This should really be done at menu setup rather
   * than at scan, since it's so expensive to scan. But that's big changes
   * to browser.c */
  if (!((regexec (Mask.rx, relpath, 0, NULL, 0) == 0) ^ Mask.not))
  {
    FREE (&mx.mbox);
    return;
  }

  imap_qualify_path (tmp, sizeof (tmp), &mx, folder);
  (state->entry)[state->entrylen].full_path = safe_strdup (tmp);

  /* mark desc with delim in browser if it can have subfolders */
  if (!isparent && !noinferiors && strlen (relpath) < sizeof (relpath) - 1)
  {
    relpath[strlen (relpath) + 1] = '\0';
    relpath[strlen (relpath)] = delim;
  }

  (state->entry)[state->entrylen].display_name = safe_strdup (relpath);

  (state->entry)[state->entrylen].number = state->entrylen;

  (state->entry)[state->entrylen].imap = 1;
  /* delimiter at the root is useless. */
  if (folder[0] == '\0')
    delim = '\0';
  (state->entry)[state->entrylen].delim = delim;
  (state->entry)[state->entrylen].selectable = !noselect;
  (state->entry)[state->entrylen].inferiors = !noinferiors;

  b = Incoming;
  while (b && mutt_strcmp (tmp, mutt_b2s (b->pathbuf)))
    b = b->next;
  if (b)
  {
    if (Context && !b->nopoll &&
        !mutt_strcmp (b->realpath, Context->realpath))
    {
      b->msg_count = Context->msgcount;
      b->msg_unread = Context->unread;
    }
    (state->entry)[state->entrylen].has_buffy = 1;
    (state->entry)[state->entrylen].new = b->new;
    (state->entry)[state->entrylen].msg_count = b->msg_count;
    (state->entry)[state->entrylen].msg_unread = b->msg_unread;
  }

  (state->entrylen)++;

  FREE (&mx.mbox);
}

static int compare_names(struct folder_file *a, struct folder_file *b)
{
  return mutt_strcmp(a->full_path, b->full_path);
}
