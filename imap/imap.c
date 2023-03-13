/*
 * Copyright (C) 1996-1998,2012 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1996-1999 Brandon Long <blong@fiction.net>
 * Copyright (C) 1999-2009,2012,2017 Brendan Cully <brendan@kublai.com>
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

/* Support for IMAP4rev1, with the occasional nod to IMAP 4. */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "mx.h"
#include "mailbox.h"
#include "globals.h"
#include "sort.h"
#include "browser.h"
#include "imap_private.h"
#if defined(USE_SSL)
# include "mutt_ssl.h"
#endif
#if defined(USE_ZLIB)
# include "mutt_zstrm.h"
#endif
#include "buffy.h"
#if USE_HCACHE
#include "hcache.h"
#endif

#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

/* imap forward declarations */
static char* imap_get_flags (LIST** hflags, char* s);
static int imap_check_capabilities (IMAP_DATA* idata);
static void imap_set_flag (IMAP_DATA* idata, int aclbit, int flag,
			   const char* str, char* flags, size_t flsize);

/* imap_access: Check permissions on an IMAP mailbox.
 * TODO: ACL checks. Right now we assume if it exists we can
 *       mess with it. */
int imap_access (const char* path)
{
  IMAP_DATA* idata;
  IMAP_MBOX mx;
  char buf[LONG_STRING*2];
  char mailbox[LONG_STRING];
  char mbox[LONG_STRING];
  int rc;

  if (imap_parse_path (path, &mx))
    return -1;

  if (!(idata = imap_conn_find (&mx.account,
                                option (OPTIMAPPASSIVE) ? MUTT_IMAP_CONN_NONEW : 0)))
  {
    FREE (&mx.mbox);
    return -1;
  }

  imap_fix_path (idata, mx.mbox, mailbox, sizeof (mailbox));
  if (!*mailbox)
    strfcpy (mailbox, "INBOX", sizeof (mailbox));

  /* we may already be in the folder we're checking */
  if (!ascii_strcmp(idata->mailbox, mx.mbox))
  {
    FREE (&mx.mbox);
    return 0;
  }
  FREE (&mx.mbox);

  if (imap_mboxcache_get (idata, mailbox, 0))
  {
    dprint (3, (debugfile, "imap_access: found %s in cache\n", mailbox));
    return 0;
  }

  imap_munge_mbox_name (idata, mbox, sizeof (mbox), mailbox);

  if (mutt_bit_isset (idata->capabilities, IMAP4REV1))
    snprintf (buf, sizeof (buf), "STATUS %s (UIDVALIDITY)", mbox);
  else if (mutt_bit_isset (idata->capabilities, STATUS))
    snprintf (buf, sizeof (buf), "STATUS %s (UID-VALIDITY)", mbox);
  else
  {
    dprint (2, (debugfile, "imap_access: STATUS not supported?\n"));
    return -1;
  }

  if ((rc = imap_exec (idata, buf, IMAP_CMD_FAIL_OK)) < 0)
  {
    dprint (1, (debugfile, "imap_access: Can't check STATUS of %s\n", mbox));
    return rc;
  }

  return 0;
}

int imap_create_mailbox (IMAP_DATA* idata, char* mailbox)
{
  char buf[LONG_STRING*2], mbox[LONG_STRING];

  imap_munge_mbox_name (idata, mbox, sizeof (mbox), mailbox);
  snprintf (buf, sizeof (buf), "CREATE %s", mbox);

  if (imap_exec (idata, buf, 0) != 0)
  {
    mutt_error (_("CREATE failed: %s"), imap_cmd_trailer (idata));
    return -1;
  }

  return 0;
}

int imap_rename_mailbox (IMAP_DATA* idata, IMAP_MBOX* mx, const char* newname)
{
  char oldmbox[LONG_STRING];
  char newmbox[LONG_STRING];
  BUFFER *b;
  int rc = 0;

  imap_munge_mbox_name (idata, oldmbox, sizeof (oldmbox), mx->mbox);
  imap_munge_mbox_name (idata, newmbox, sizeof (newmbox), newname);

  b = mutt_buffer_pool_get ();
  mutt_buffer_printf (b, "RENAME %s %s", oldmbox, newmbox);

  if (imap_exec (idata, mutt_b2s (b), 0) != 0)
    rc = -1;

  mutt_buffer_pool_release (&b);

  return rc;
}

int imap_delete_mailbox (CONTEXT* ctx, IMAP_MBOX mx)
{
  char buf[LONG_STRING*2], mbox[LONG_STRING];
  IMAP_DATA *idata;

  if (!ctx || !ctx->data)
  {
    if (!(idata = imap_conn_find (&mx.account,
                                  option (OPTIMAPPASSIVE) ? MUTT_IMAP_CONN_NONEW : 0)))
    {
      FREE (&mx.mbox);
      return -1;
    }
  }
  else
  {
    idata = ctx->data;
  }

  imap_munge_mbox_name (idata, mbox, sizeof (mbox), mx.mbox);
  snprintf (buf, sizeof (buf), "DELETE %s", mbox);

  if (imap_exec ((IMAP_DATA*) idata, buf, 0) != 0)
    return -1;

  return 0;
}

/* imap_logout_all: close all open connections. Quick and dirty until we can
 *   make sure we've got all the context we need. */
void imap_logout_all (void)
{
  CONNECTION* conn;
  CONNECTION* tmp;

  conn = mutt_socket_head ();

  while (conn)
  {
    tmp = conn->next;

    if (conn->account.type == MUTT_ACCT_TYPE_IMAP && conn->fd >= 0)
    {
      mutt_message (_("Closing connection to %s..."), conn->account.host);
      imap_logout ((IMAP_DATA**) (void*) &conn->data);
      mutt_clear_error ();
      mutt_socket_free (conn);
    }

    conn = tmp;
  }
}

/* imap_read_literal: read bytes bytes from server into file. Not explicitly
 *   buffered, relies on FILE buffering. */
int imap_read_literal (FILE* fp, IMAP_DATA* idata, unsigned int bytes, progress_t* pbar)
{
  unsigned int pos;
  char c;

  int r = 0;

  dprint (2, (debugfile, "imap_read_literal: reading %ld bytes\n", bytes));

  for (pos = 0; pos < bytes; pos++)
  {
    if (mutt_socket_readchar (idata->conn, &c) != 1)
    {
      dprint (1, (debugfile, "imap_read_literal: error during read, %ld bytes read\n", pos));
      idata->status = IMAP_FATAL;

      return -1;
    }

    /* Strip \r from \r\n, apparantly even literals use \r\n-terminated
      strings ?! */
    if (r == 1 && c != '\n')
      fputc ('\r', fp);

    if (c == '\r')
    {
      r = 1;
      continue;
    }
    else
      r = 0;

    fputc (c, fp);

    if (pbar && !(pos % 1024))
      mutt_progress_update (pbar, pos, -1);
#ifdef DEBUG
    if (debuglevel >= IMAP_LOG_LTRL)
      fputc (c, debugfile);
#endif
  }

  return 0;
}

/* imap_expunge_mailbox: Purge IMAP portion of expunged messages from the
 *   context. Must not be done while something has a handle on any headers
 *   (eg inside pager or editor). That is, check IMAP_REOPEN_ALLOW. */
void imap_expunge_mailbox (IMAP_DATA* idata)
{
  HEADER* h;
  int i, cacheno;
  short old_sort;

#ifdef USE_HCACHE
  idata->hcache = imap_hcache_open (idata, NULL);
#endif

  old_sort = Sort;
  Sort = SORT_ORDER;
  mutt_sort_headers (idata->ctx, 0);

  for (i = 0; i < idata->ctx->msgcount; i++)
  {
    h = idata->ctx->hdrs[i];

    if (h->index == INT_MAX)
    {
      dprint (2, (debugfile, "Expunging message UID %u.\n", HEADER_DATA (h)->uid));

      h->active = 0;
      idata->ctx->size -= h->content->length;

      imap_cache_del (idata, h);
#if USE_HCACHE
      imap_hcache_del (idata, HEADER_DATA(h)->uid);
#endif

      /* free cached body from disk, if necessary */
      cacheno = HEADER_DATA(h)->uid % IMAP_CACHE_LEN;
      if (idata->cache[cacheno].uid == HEADER_DATA(h)->uid &&
	  idata->cache[cacheno].path)
      {
	unlink (idata->cache[cacheno].path);
	FREE (&idata->cache[cacheno].path);
      }

      int_hash_delete (idata->uid_hash, HEADER_DATA(h)->uid, h, NULL);

      imap_free_header_data ((IMAP_HEADER_DATA**)&h->data);
    }
    else
    {
      h->index = i;
      /* Mutt has several places where it turns off h->active as a
       * hack.  For example to avoid FLAG updates, or to exclude from
       * imap_exec_msgset.
       *
       * Unfortunately, when a reopen is allowed and the IMAP_EXPUNGE_PENDING
       * flag becomes set (e.g. a flag update to a modified header),
       * this function will be called by imap_cmd_finish().
       *
       * The mx_update_tables() will free and remove these "inactive" headers,
       * despite that an EXPUNGE was not received for them.
       * This would result in memory leaks and segfaults due to dangling
       * pointers in the msn_index and uid_hash.
       *
       * So this is another hack to work around the hacks.  We don't want to
       * remove the messages, so make sure active is on.
       */
      h->active = 1;
    }
  }

#if USE_HCACHE
  imap_hcache_close (idata);
#endif

  /* We may be called on to expunge at any time. We can't rely on the caller
   * to always know to rethread */
  mx_update_tables (idata->ctx, 0);
  Sort = old_sort;
  mutt_sort_headers (idata->ctx, 1);
}

/* imap_check_capabilities: make sure we can log in to this server. */
static int imap_check_capabilities (IMAP_DATA* idata)
{
  if (imap_exec (idata, "CAPABILITY", 0) != 0)
  {
    imap_error ("imap_check_capabilities", idata->buf);
    return -1;
  }

  if (!(mutt_bit_isset(idata->capabilities,IMAP4) ||
        mutt_bit_isset(idata->capabilities,IMAP4REV1)))
  {
    mutt_error _("This IMAP server is ancient. Mutt does not work with it.");
    mutt_sleep (2);	/* pause a moment to let the user see the error */

    return -1;
  }

  return 0;
}

/**
 * imap_conn_find
 *
 * Returns an authenticated IMAP connection matching account, or NULL
 * if that isn't possible.
 *
 * flags:
 *   MUTT_IMAP_CONN_NONEW    - must be an existing connection
 *   MUTT_IMAP_CONN_NOSELECT - must not be in the IMAP_SELECTED state.
 */
IMAP_DATA* imap_conn_find (const ACCOUNT* account, int flags)
{
  CONNECTION* conn = NULL;
  ACCOUNT* creds = NULL;
  IMAP_DATA* idata = NULL;
  int new = 0;

  while ((conn = mutt_conn_find (conn, account)))
  {
    if (!creds)
      creds = &conn->account;
    else
      memcpy (&conn->account, creds, sizeof (ACCOUNT));

    idata = (IMAP_DATA*)conn->data;
    if (flags & MUTT_IMAP_CONN_NONEW)
    {
      if (!idata)
      {
        /* This should only happen if we've come to the end of the list */
        mutt_socket_free (conn);
        return NULL;
      }
      else if (idata->state < IMAP_AUTHENTICATED)
        continue;
    }
    if (flags & MUTT_IMAP_CONN_NOSELECT && idata && idata->state >= IMAP_SELECTED)
      continue;
    if (idata && idata->status == IMAP_FATAL)
      continue;
    break;
  }
  if (!conn)
    return NULL; /* this happens when the initial connection fails */

  /* The current connection is a new connection */
  if (!idata)
  {
    idata = imap_new_idata ();
    conn->data = idata;
    idata->conn = conn;
    new = 1;
  }

  if (idata->state == IMAP_DISCONNECTED)
    imap_open_connection (idata);
  if (idata->state == IMAP_CONNECTED)
  {
    if (!imap_authenticate (idata))
    {
      idata->state = IMAP_AUTHENTICATED;
      FREE (&idata->capstr);
      new = 1;
      if (idata->conn->ssf)
	dprint (2, (debugfile, "Communication encrypted at %d bits\n",
		    idata->conn->ssf));
    }
    else
      mutt_account_unsetpass (&idata->conn->account);
  }
  if (new && idata->state == IMAP_AUTHENTICATED)
  {
    /* capabilities may have changed */
    imap_exec (idata, "CAPABILITY", IMAP_CMD_FAIL_OK);

#if defined(USE_ZLIB)
    /* RFC 4978 */
    if (mutt_bit_isset (idata->capabilities, COMPRESS_DEFLATE))
    {
      if (option (OPTIMAPDEFLATE) &&
	  imap_exec (idata, "COMPRESS DEFLATE", IMAP_CMD_FAIL_OK) == 0)
	mutt_zstrm_wrap_conn (idata->conn);
    }
#endif

    /* enable RFC6855, if the server supports that */
    if (mutt_bit_isset (idata->capabilities, ENABLE))
      imap_exec (idata, "ENABLE UTF8=ACCEPT", IMAP_CMD_QUEUE);

    /* enable QRESYNC.  Advertising QRESYNC also means CONDSTORE
     * is supported (even if not advertised), so flip that bit. */
    if (mutt_bit_isset (idata->capabilities, QRESYNC))
    {
      mutt_bit_set (idata->capabilities, CONDSTORE);
      if (option (OPTIMAPQRESYNC))
        imap_exec (idata, "ENABLE QRESYNC", IMAP_CMD_QUEUE);
    }

    /* get root delimiter, '/' as default */
    idata->delim = '/';
    imap_exec (idata, "LIST \"\" \"\"", IMAP_CMD_QUEUE);
    if (option (OPTIMAPCHECKSUBSCRIBED))
      imap_exec (idata, "LSUB \"\" \"*\"", IMAP_CMD_QUEUE);

    /* we may need the root delimiter before we open a mailbox */
    imap_exec (idata, NULL, IMAP_CMD_FAIL_OK);
  }

  if (idata->state < IMAP_AUTHENTICATED)
    return NULL;

  return idata;
}

int imap_open_connection (IMAP_DATA* idata)
{
  if (mutt_socket_open (idata->conn) < 0)
    return -1;

  idata->state = IMAP_CONNECTED;

  if (imap_cmd_step (idata) != IMAP_CMD_OK)
  {
    imap_close_connection (idata);
    return -1;
  }

  if (ascii_strncasecmp ("* OK", idata->buf, 4) == 0)
  {
    if (ascii_strncasecmp ("* OK [CAPABILITY", idata->buf, 16)
        && imap_check_capabilities (idata))
      goto bail;
#if defined(USE_SSL)
    /* Attempt STARTTLS if available and desired. */
    if (!idata->conn->ssf && (option(OPTSSLFORCETLS) ||
                              mutt_bit_isset (idata->capabilities, STARTTLS)))
    {
      int rc;

      if (option(OPTSSLFORCETLS))
        rc = MUTT_YES;
      else if ((rc = query_quadoption (OPT_SSLSTARTTLS,
                                       _("Secure connection with TLS?"))) == -1)
	goto bail;
      if (rc == MUTT_YES)
      {
	if ((rc = imap_exec (idata, "STARTTLS", IMAP_CMD_FAIL_OK)) == -1)
	  goto bail;
	if (rc != -2)
	{
	  if (mutt_ssl_starttls (idata->conn))
	  {
	    mutt_error (_("Could not negotiate TLS connection"));
	    mutt_sleep (1);
	    goto bail;
	  }
	  else
	  {
	    /* RFC 2595 demands we recheck CAPABILITY after TLS completes. */
	    if (imap_exec (idata, "CAPABILITY", 0))
	      goto bail;
	  }
	}
      }
    }

    if (option(OPTSSLFORCETLS) && ! idata->conn->ssf)
    {
      mutt_error _("Encrypted connection unavailable");
      mutt_sleep (1);
      goto bail;
    }
#endif
  }
  else if (ascii_strncasecmp ("* PREAUTH", idata->buf, 9) == 0)
  {
#if defined(USE_SSL)
    /* Unless using a secure $tunnel, an unencrypted PREAUTH response
     * may be a MITM attack.  The only way to stop "STARTTLS" MITM
     * attacks is via $ssl_force_tls: an attacker can easily spoof
     * "* OK" and strip the STARTTLS capability.  So consult
     * $ssl_force_tls, not $ssl_starttls, to decide whether to
     * abort. Note that if using $tunnel and $tunnel_is_secure,
     * conn->ssf will be set to 1. */
    if (!idata->conn->ssf && option(OPTSSLFORCETLS))
    {
      mutt_error _("Encrypted connection unavailable");
      mutt_sleep (1);
      goto bail;
    }
#endif

    idata->state = IMAP_AUTHENTICATED;
    if (imap_check_capabilities (idata) != 0)
      goto bail;
    FREE (&idata->capstr);
  }
  else
  {
    imap_error ("imap_open_connection()", idata->buf);
    goto bail;
  }

  return 0;

bail:
  imap_close_connection (idata);
  FREE (&idata->capstr);
  return -1;
}

void imap_close_connection(IMAP_DATA* idata)
{
  if (idata->state != IMAP_DISCONNECTED)
  {
    mutt_socket_close (idata->conn);
    idata->state = IMAP_DISCONNECTED;
  }
  idata->seqno = idata->nextcmd = idata->lastcmd = idata->status = 0;
  memset (idata->cmds, 0, sizeof (IMAP_COMMAND) * idata->cmdslots);
}

/* Try to reconnect and merge current state back in.
 * This is only done currently during mx_check_mailbox() polling
 * when reopen is allowed. */
int imap_reconnect (IMAP_DATA **p_idata)
{
  CONTEXT *orig_ctx, new_ctx;
  int rc = -1, i;
  IMAP_DATA *idata = *p_idata;
  HEADER *old_hdr, *new_hdr;

  /* L10N:
     Message displayed when IMAP connection is lost and Mutt
     tries to reconnect.
  */
  mutt_message _("Trying to reconnect...");
  mutt_sleep (0);

  orig_ctx = idata->ctx;
  if (!orig_ctx)
    goto cleanup;

  if (mx_open_mailbox (orig_ctx->path,
                       orig_ctx->readonly ? MUTT_READONLY : 0,
                       &new_ctx) == NULL)
    goto cleanup;

  new_ctx.dontwrite = orig_ctx->dontwrite;
  new_ctx.pattern = orig_ctx->pattern;
  new_ctx.limit_pattern = orig_ctx->limit_pattern;

  orig_ctx->pattern = NULL;
  orig_ctx->limit_pattern = NULL;

  if (idata->uid_validity == ((IMAP_DATA *) new_ctx.data)->uid_validity)
  {
    for (i = 0; i < new_ctx.msgcount; i++)
    {
      new_hdr = new_ctx.hdrs[i];
      old_hdr = (HEADER *) int_hash_find (idata->uid_hash,
                                          HEADER_DATA(new_hdr)->uid);
      if (!old_hdr)
        continue;

      /* this logic is in part from mbox.c. */
      if (old_hdr->changed)
      {
        mutt_set_flag (&new_ctx, new_hdr, MUTT_FLAG, old_hdr->flagged);
        mutt_set_flag (&new_ctx, new_hdr, MUTT_REPLIED, old_hdr->replied);
        mutt_set_flag (&new_ctx, new_hdr, MUTT_OLD, old_hdr->old);
        mutt_set_flag (&new_ctx, new_hdr, MUTT_READ, old_hdr->read);

        /* TODO: the ->env check is unfortunately voodoo that I
         * haven't taken the time to track down yet.  It's in other
         * parts of the code but I don't know why yet. */
        if (old_hdr->env && old_hdr->env->changed)
        {
          new_hdr->env->changed = old_hdr->env->changed;
          new_hdr->changed = 1;
          new_ctx.changed = 1;

          if (old_hdr->env->changed & MUTT_ENV_CHANGED_IRT)
          {
            mutt_free_list (&new_hdr->env->in_reply_to);
            new_hdr->env->in_reply_to = old_hdr->env->in_reply_to;
            old_hdr->env->in_reply_to = NULL;
          }
          if (old_hdr->env->changed & MUTT_ENV_CHANGED_REFS)
          {
            mutt_free_list (&new_hdr->env->references);
            new_hdr->env->references = old_hdr->env->references;
            old_hdr->env->references = NULL;
          }
          if (old_hdr->env->changed & MUTT_ENV_CHANGED_XLABEL)
          {
            FREE (&new_hdr->env->x_label);
            new_hdr->env->x_label = old_hdr->env->x_label;
            old_hdr->env->x_label = NULL;
          }
          if (old_hdr->env->changed & MUTT_ENV_CHANGED_SUBJECT)
          {
            FREE (&new_hdr->env->subject);
            new_hdr->env->subject = old_hdr->env->subject;
            new_hdr->env->real_subj = old_hdr->env->real_subj;
            old_hdr->env->subject = old_hdr->env->real_subj = NULL;
          }
        }

        if (old_hdr->attach_del)
        {
          if (old_hdr->content->parts && !new_hdr->content->parts)
          {
            new_hdr->attach_del = 1;
            new_hdr->changed = 1;
            new_ctx.changed = 1;
            new_hdr->content->parts = old_hdr->content->parts;
            old_hdr->content->parts = NULL;
          }
        }
      }

      mutt_set_flag (&new_ctx, new_hdr, MUTT_DELETE, old_hdr->deleted);
      mutt_set_flag (&new_ctx, new_hdr, MUTT_PURGE, old_hdr->purge);
      mutt_set_flag (&new_ctx, new_hdr, MUTT_TAG, old_hdr->tagged);
    }
  }

  rc = 0;

cleanup:
  idata->status = IMAP_FATAL;
  mx_fastclose_mailbox (orig_ctx);
  imap_close_connection (idata);

  if (rc != 0)
  {
    /* L10N:
       Message when Mutt tries to reconnect to an IMAP mailbox but is
       unable to.
    */
    mutt_error _("Reconnect failed.  Mailbox closed.");
  }
  else
  {
    memcpy (orig_ctx, &new_ctx, sizeof(CONTEXT));
    idata = (IMAP_DATA *)orig_ctx->data;
    idata->ctx = orig_ctx;
    *p_idata = idata;
    /* L10N:
       Message when Mutt reconnects to an IMAP mailbox after a fatal error.
    */
    mutt_error _("Reconnect succeeded.");
  }
  mutt_sleep (0);

  return rc;
}

/* imap_get_flags: Make a simple list out of a FLAGS response.
 *   return stream following FLAGS response */
static char* imap_get_flags (LIST** hflags, char* s)
{
  LIST* flags;
  char* flag_word;
  char ctmp;

  /* sanity-check string */
  if (ascii_strncasecmp ("FLAGS", s, 5) != 0)
  {
    dprint (1, (debugfile, "imap_get_flags: not a FLAGS response: %s\n",
                s));
    return NULL;
  }
  s += 5;
  SKIPWS(s);
  if (*s != '(')
  {
    dprint (1, (debugfile, "imap_get_flags: bogus FLAGS response: %s\n",
                s));
    return NULL;
  }

  /* create list, update caller's flags handle */
  flags = mutt_new_list();
  *hflags = flags;

  while (*s && *s != ')')
  {
    s++;
    SKIPWS(s);
    flag_word = s;
    while (*s && (*s != ')') && !ISSPACE (*s))
      s++;
    ctmp = *s;
    *s = '\0';
    if (*flag_word)
      mutt_add_list (flags, flag_word);
    *s = ctmp;
  }

  /* note bad flags response */
  if (*s != ')')
  {
    dprint (1, (debugfile,
                "imap_get_flags: Unterminated FLAGS response: %s\n", s));
    mutt_free_list (hflags);

    return NULL;
  }

  s++;

  return s;
}

static int imap_open_mailbox (CONTEXT* ctx)
{
  IMAP_DATA *idata;
  IMAP_STATUS* status;
  char buf[LONG_STRING];
  char bufout[LONG_STRING*2];
  int count = 0;
  IMAP_MBOX mx, pmx;
  int rc;
  const char *condstore;

  if (imap_parse_path (ctx->path, &mx))
  {
    mutt_error (_("%s is an invalid IMAP path"), ctx->path);
    return -1;
  }

  /* we require a connection which isn't currently in IMAP_SELECTED state */
  if (!(idata = imap_conn_find (&(mx.account), MUTT_IMAP_CONN_NOSELECT)))
    goto fail_noidata;

  /* once again the context is new */
  ctx->data = idata;

  /* Clean up path and replace the one in the ctx */
  imap_fix_path (idata, mx.mbox, buf, sizeof (buf));
  if (!*buf)
    strfcpy (buf, "INBOX", sizeof (buf));
  FREE(&(idata->mailbox));
  idata->mailbox = safe_strdup (buf);
  imap_qualify_path (buf, sizeof (buf), &mx, idata->mailbox);

  FREE (&(ctx->path));
  FREE (&(ctx->realpath));
  ctx->path = safe_strdup (buf);
  ctx->realpath = safe_strdup (ctx->path);

  idata->ctx = ctx;

  /* clear mailbox status */
  idata->status = 0;
  memset (idata->ctx->rights, 0, sizeof (idata->ctx->rights));
  idata->newMailCount = 0;
  idata->max_msn = 0;

  if (!ctx->quiet)
    mutt_message (_("Selecting %s..."), idata->mailbox);
  imap_munge_mbox_name (idata, buf, sizeof(buf), idata->mailbox);

  /* pipeline ACL test */
  if (mutt_bit_isset (idata->capabilities, ACL))
  {
    snprintf (bufout, sizeof (bufout), "MYRIGHTS %s", buf);
    imap_exec (idata, bufout, IMAP_CMD_QUEUE);
  }
  /* assume we have all rights if ACL is unavailable */
  else
  {
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_LOOKUP);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_READ);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_SEEN);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_WRITE);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_INSERT);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_POST);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_CREATE);
    mutt_bit_set (idata->ctx->rights, MUTT_ACL_DELETE);
  }
  /* pipeline the postponed count if possible */
  pmx.mbox = NULL;
  if (mx_is_imap (Postponed) && !imap_parse_path (Postponed, &pmx)
      && mutt_account_match (&pmx.account, &mx.account))
    imap_status (Postponed, 1);
  FREE (&pmx.mbox);

#if USE_HCACHE
  if (mutt_bit_isset (idata->capabilities, CONDSTORE) &&
      option (OPTIMAPCONDSTORE))
    condstore = " (CONDSTORE)";
  else
#endif
    condstore = "";

  snprintf (bufout, sizeof (bufout), "%s %s%s",
            ctx->readonly ? "EXAMINE" : "SELECT",
            buf, condstore);

  idata->state = IMAP_SELECTED;

  imap_cmd_start (idata, bufout);

  status = imap_mboxcache_get (idata, idata->mailbox, 1);

  do
  {
    char *pc;

    if ((rc = imap_cmd_step (idata)) != IMAP_CMD_CONTINUE)
      break;

    if (ascii_strncmp (idata->buf, "* ", 2))
      continue;
    pc = imap_next_word (idata->buf);

    /* Obtain list of available flags here, may be overridden by a
     * PERMANENTFLAGS tag in the OK response */
    if (ascii_strncasecmp ("FLAGS", pc, 5) == 0)
    {
      /* don't override PERMANENTFLAGS */
      if (!idata->flags)
      {
	dprint (3, (debugfile, "Getting mailbox FLAGS\n"));
	if ((pc = imap_get_flags (&(idata->flags), pc)) == NULL)
	  goto fail;
      }
    }
    /* PERMANENTFLAGS are massaged to look like FLAGS, then override FLAGS */
    else if (ascii_strncasecmp ("OK [PERMANENTFLAGS", pc, 18) == 0)
    {
      dprint (3, (debugfile, "Getting mailbox PERMANENTFLAGS\n"));
      /* safe to call on NULL */
      mutt_free_list (&(idata->flags));
      /* skip "OK [PERMANENT" so syntax is the same as FLAGS */
      pc += 13;
      if ((pc = imap_get_flags (&(idata->flags), pc)) == NULL)
	goto fail;
    }
    /* save UIDVALIDITY for the header cache */
    else if (ascii_strncasecmp ("OK [UIDVALIDITY", pc, 14) == 0)
    {
      dprint (3, (debugfile, "Getting mailbox UIDVALIDITY\n"));
      pc += 3;
      pc = imap_next_word (pc);
      if (mutt_atoui (pc, &idata->uid_validity, MUTT_ATOI_ALLOW_TRAILING) < 0)
        goto fail;
      status->uidvalidity = idata->uid_validity;
    }
    else if (ascii_strncasecmp ("OK [UIDNEXT", pc, 11) == 0)
    {
      dprint (3, (debugfile, "Getting mailbox UIDNEXT\n"));
      pc += 3;
      pc = imap_next_word (pc);
      if (mutt_atoui (pc, &idata->uidnext, MUTT_ATOI_ALLOW_TRAILING) < 0)
        goto fail;
      status->uidnext = idata->uidnext;
    }
    else if (ascii_strncasecmp ("OK [HIGHESTMODSEQ", pc, 17) == 0)
    {
      dprint (3, (debugfile, "Getting mailbox HIGHESTMODSEQ\n"));
      pc += 3;
      pc = imap_next_word (pc);
      if (mutt_atoull (pc, &idata->modseq, MUTT_ATOI_ALLOW_TRAILING) < 0)
        goto fail;
      status->modseq = idata->modseq;
    }
    else if (ascii_strncasecmp ("OK [NOMODSEQ", pc, 12) == 0)
    {
      dprint (3, (debugfile, "Mailbox has NOMODSEQ set\n"));
      status->modseq = idata->modseq = 0;
    }
    else
    {
      pc = imap_next_word (pc);
      if (!ascii_strncasecmp ("EXISTS", pc, 6))
      {
	count = idata->newMailCount;
	idata->newMailCount = 0;
      }
    }
  }
  while (rc == IMAP_CMD_CONTINUE);

  if (rc == IMAP_CMD_NO)
  {
    char *s;
    s = imap_next_word (idata->buf); /* skip seq */
    s = imap_next_word (s); /* Skip response */
    mutt_error ("%s", s);
    mutt_sleep (2);
    goto fail;
  }

  if (rc != IMAP_CMD_OK)
    goto fail;

  /* check for READ-ONLY notification */
  if (!ascii_strncasecmp (imap_get_qualifier (idata->buf), "[READ-ONLY]", 11) &&
      !mutt_bit_isset (idata->capabilities, ACL))
  {
    dprint (2, (debugfile, "Mailbox is read-only.\n"));
    ctx->readonly = 1;
  }

#ifdef DEBUG
  /* dump the mailbox flags we've found */
  if (debuglevel > 2)
  {
    if (!idata->flags)
      dprint (3, (debugfile, "No folder flags found\n"));
    else
    {
      LIST* t = idata->flags;

      dprint (3, (debugfile, "Mailbox flags: "));

      t = t->next;
      while (t)
      {
        dprint (3, (debugfile, "[%s] ", t->data));
        t = t->next;
      }
      dprint (3, (debugfile, "\n"));
    }
  }
#endif

  if (!(mutt_bit_isset(idata->ctx->rights, MUTT_ACL_DELETE) ||
        mutt_bit_isset(idata->ctx->rights, MUTT_ACL_SEEN) ||
        mutt_bit_isset(idata->ctx->rights, MUTT_ACL_WRITE) ||
        mutt_bit_isset(idata->ctx->rights, MUTT_ACL_INSERT)))
    ctx->readonly = 1;

  ctx->hdrmax = count;
  ctx->hdrs = safe_calloc (count, sizeof (HEADER *));
  ctx->v2r = safe_calloc (count, sizeof (int));
  ctx->msgcount = 0;

  if (count && (imap_read_headers (idata, 1, count, 1) < 0))
  {
    mutt_error _("Error opening mailbox");
    mutt_sleep (1);
    goto fail;
  }

  imap_disallow_reopen (ctx);

  dprint (2, (debugfile, "imap_open_mailbox: msgcount is %d\n", ctx->msgcount));
  FREE (&mx.mbox);
  return 0;

fail:
  if (idata->state == IMAP_SELECTED)
    idata->state = IMAP_AUTHENTICATED;
fail_noidata:
  FREE (&mx.mbox);
  return -1;
}

static int imap_open_mailbox_append (CONTEXT *ctx, int flags)
{
  IMAP_DATA *idata;
  char buf[LONG_STRING];
  char mailbox[LONG_STRING];
  IMAP_MBOX mx;
  int rc;

  if (imap_parse_path (ctx->path, &mx))
    return -1;

  /* in APPEND mode, we appear to hijack an existing IMAP connection -
   * ctx is brand new and mostly empty */

  if (!(idata = imap_conn_find (&(mx.account), 0)))
  {
    FREE (&mx.mbox);
    return -1;
  }

  ctx->data = idata;

  imap_fix_path (idata, mx.mbox, mailbox, sizeof (mailbox));
  if (!*mailbox)
    strfcpy (mailbox, "INBOX", sizeof (mailbox));
  FREE (&mx.mbox);

  if ((rc = imap_access (ctx->path)) == 0)
    return 0;

  if (rc == -1)
    return -1;

  if (option (OPTCONFIRMCREATE))
  {
    if (option (OPTNOCURSES))
      return -1;

    snprintf (buf, sizeof (buf), _("Create %s?"), mailbox);
    if (mutt_query_boolean (OPTCONFIRMCREATE, buf, 1) < 1)
      return -1;
  }

  if (imap_create_mailbox (idata, mailbox) < 0)
    return -1;

  return 0;
}

/* imap_logout: Gracefully log out of server. */
void imap_logout (IMAP_DATA** idata)
{
  /* we set status here to let imap_handle_untagged know we _expect_ to
   * receive a bye response (so it doesn't freak out and close the conn) */
  (*idata)->status = IMAP_BYE;
  imap_cmd_start (*idata, "LOGOUT");
  if (ImapPollTimeout <= 0 ||
      mutt_socket_poll ((*idata)->conn, ImapPollTimeout) != 0)
  {
    while (imap_cmd_step (*idata) == IMAP_CMD_CONTINUE)
      ;
  }

  mutt_socket_close ((*idata)->conn);
  imap_free_idata (idata);
}

static int imap_open_new_message (MESSAGE *msg, CONTEXT *dest, HEADER *hdr)
{
  BUFFER *tmp = NULL;
  int rc = -1;

  tmp = mutt_buffer_pool_get ();
  mutt_buffer_mktemp (tmp);
  if ((msg->fp = safe_fopen (mutt_b2s (tmp), "w")) == NULL)
  {
    mutt_perror (mutt_b2s (tmp));
    goto cleanup;
  }

  msg->path = safe_strdup (mutt_b2s (tmp));
  rc = 0;

cleanup:
  mutt_buffer_pool_release (&tmp);
  return rc;
}

/* imap_set_flag: append str to flags if we currently have permission
 *   according to aclbit */
static void imap_set_flag (IMAP_DATA* idata, int aclbit, int flag,
                           const char *str, char *flags, size_t flsize)
{
  if (mutt_bit_isset (idata->ctx->rights, aclbit))
    if (flag && imap_has_flag (idata->flags, str))
      safe_strcat (flags, flsize, str);
}

/* imap_has_flag: do a caseless comparison of the flag against a flag list,
*   return 1 if found or flag list has '\*', 0 otherwise */
int imap_has_flag (LIST* flag_list, const char* flag)
{
  if (!flag_list)
    return 0;

  flag_list = flag_list->next;
  while (flag_list)
  {
    if (!ascii_strncasecmp (flag_list->data, flag, strlen (flag_list->data)))
      return 1;

    if (!ascii_strncmp (flag_list->data, "\\*", strlen (flag_list->data)))
      return 1;

    flag_list = flag_list->next;
  }

  return 0;
}

static int compare_uid (const void *a, const void *b)
{
  HEADER **pa = (HEADER **) a;
  HEADER **pb = (HEADER **) b;

  return mutt_numeric_cmp (HEADER_DATA(*pa)->uid, HEADER_DATA(*pb)->uid);
}

/* Note: headers must be in SORT_UID. See imap_exec_msgset for args.
 * Pos is an opaque pointer a la strtok. It should be 0 at first call. */
static int imap_make_msg_set (IMAP_DATA* idata, BUFFER* buf, int flag,
                              int changed, int invert, int* pos)
{
  HEADER** hdrs = idata->ctx->hdrs;
  int count = 0;	/* number of messages in message set */
  int match = 0;	/* whether current message matches flag condition */
  unsigned int setstart = 0;	/* start of current message range */
  int n;
  int started = 0;

  hdrs = idata->ctx->hdrs;

  for (n = *pos;
       (n < idata->ctx->msgcount) && (mutt_buffer_len (buf) < IMAP_MAX_CMDLEN);
       n++)
  {
    match = 0;
    /* don't include pending expunged messages.
     *
     * TODO: can we unset active in cmd_parse_expunge() and
     * cmd_parse_vanished() instead of checking for index != INT_MAX. */
    if (hdrs[n]->active && (hdrs[n]->index != INT_MAX))
      switch (flag)
      {
        case MUTT_DELETED:
          if (hdrs[n]->deleted != HEADER_DATA(hdrs[n])->deleted)
            match = invert ^ hdrs[n]->deleted;
	  break;
        case MUTT_FLAG:
          if (hdrs[n]->flagged != HEADER_DATA(hdrs[n])->flagged)
            match = invert ^ hdrs[n]->flagged;
	  break;
        case MUTT_OLD:
          if (hdrs[n]->old != HEADER_DATA(hdrs[n])->old)
            match = invert ^ hdrs[n]->old;
	  break;
        case MUTT_READ:
          if (hdrs[n]->read != HEADER_DATA(hdrs[n])->read)
            match = invert ^ hdrs[n]->read;
	  break;
        case MUTT_REPLIED:
          if (hdrs[n]->replied != HEADER_DATA(hdrs[n])->replied)
            match = invert ^ hdrs[n]->replied;
	  break;

        case MUTT_TAG:
	  if (hdrs[n]->tagged)
	    match = 1;
	  break;
        case MUTT_TRASH:
          if (hdrs[n]->deleted && !hdrs[n]->purge)
            match = 1;
	  break;
      }

    if (match && (!changed || hdrs[n]->changed))
    {
      count++;
      if (setstart == 0)
      {
        setstart = HEADER_DATA (hdrs[n])->uid;
        if (started == 0)
	{
	  mutt_buffer_add_printf (buf, "%u", HEADER_DATA (hdrs[n])->uid);
	  started = 1;
	}
        else
	  mutt_buffer_add_printf (buf, ",%u", HEADER_DATA (hdrs[n])->uid);
      }
      /* tie up if the last message also matches */
      else if (n == idata->ctx->msgcount-1)
	mutt_buffer_add_printf (buf, ":%u", HEADER_DATA (hdrs[n])->uid);
    }
    /* End current set if message doesn't match. */
    else if (setstart)
    {
      if (HEADER_DATA (hdrs[n-1])->uid > setstart)
	mutt_buffer_add_printf (buf, ":%u", HEADER_DATA (hdrs[n-1])->uid);
      setstart = 0;
    }
  }

  *pos = n;

  return count;
}

/* Prepares commands for all messages matching conditions (must be flushed
 * with imap_exec)
 * Params:
 *   idata: IMAP_DATA containing context containing header set
 *   pre, post: commands are of the form "%s %s %s %s", tag,
 *     pre, message set, post
 *   flag: enum of flag type on which to filter
 *   changed: include only changed messages in message set
 *   invert: invert sense of flag, eg MUTT_READ matches unread messages
 * Returns: number of matched messages, or -1 on failure */
int imap_exec_msgset (IMAP_DATA* idata, const char* pre, const char* post,
                      int flag, int changed, int invert)
{
  HEADER** hdrs = NULL;
  short oldsort;
  BUFFER* cmd;
  int pos;
  int rc;
  int count = 0, reopen_set = 0;

  cmd = mutt_buffer_new ();

  /* Unlike imap_sync_mailbox(), this function can be called when
   * IMAP_REOPEN_ALLOW is not set.  In that case, the caller isn't
   * prepared to handle context changes.  Resorting may not always
   * give the same order, so we must make a copy.
   *
   * See the comment in imap_sync_mailbox() for the dangers of running
   * even queued execs while reopen is set.  To prevent memory
   * corruption and data loss we must disable reopen for the duration
   * of the swapped hdrs.
   */
  if (idata->reopen & IMAP_REOPEN_ALLOW)
  {
    idata->reopen &= ~IMAP_REOPEN_ALLOW;
    reopen_set = 1;
  }
  oldsort = Sort;
  if (Sort != SORT_UID)
  {
    hdrs = idata->ctx->hdrs;
    idata->ctx->hdrs = safe_malloc (idata->ctx->msgcount * sizeof (HEADER*));
    memcpy (idata->ctx->hdrs, hdrs, idata->ctx->msgcount * sizeof (HEADER*));

    Sort = SORT_UID;
    qsort (idata->ctx->hdrs, idata->ctx->msgcount, sizeof (HEADER*),
           compare_uid);
  }

  pos = 0;

  do
  {
    mutt_buffer_clear (cmd);
    mutt_buffer_add_printf (cmd, "%s ", pre);
    rc = imap_make_msg_set (idata, cmd, flag, changed, invert, &pos);
    if (rc > 0)
    {
      mutt_buffer_add_printf (cmd, " %s", post);
      if (imap_exec (idata, cmd->data, IMAP_CMD_QUEUE))
      {
        rc = -1;
        goto out;
      }
      count += rc;
    }
  }
  while (rc > 0);

  rc = count;

out:
  mutt_buffer_free (&cmd);
  if ((oldsort != Sort) || hdrs)
  {
    Sort = oldsort;
    FREE (&idata->ctx->hdrs);
    idata->ctx->hdrs = hdrs;
  }
  if (reopen_set)
    idata->reopen |= IMAP_REOPEN_ALLOW;

  return rc;
}

/* returns 0 if mutt's flags match cached server flags:
 * EXCLUDING the deleted flag. */
static int compare_flags_for_copy (HEADER* h)
{
  IMAP_HEADER_DATA* hd = (IMAP_HEADER_DATA*)h->data;

  if (h->read != hd->read)
    return 1;
  if (h->old != hd->old)
    return 1;
  if (h->flagged != hd->flagged)
    return 1;
  if (h->replied != hd->replied)
    return 1;

  return 0;
}

/* Update the IMAP server to reflect the flags for a single message before
 * performing a "UID COPY".
 * NOTE: This does not sync the "deleted" flag state, because it is not
 *       desirable to propagate that flag into the copy.
 */
int imap_sync_message_for_copy (IMAP_DATA *idata, HEADER *hdr, BUFFER *cmd,
                                int *err_continue)
{
  char flags[LONG_STRING];
  char uid[11];

  if (!compare_flags_for_copy (hdr))
  {
    if (hdr->deleted == HEADER_DATA(hdr)->deleted)
      hdr->changed = 0;
    return 0;
  }

  snprintf (uid, sizeof (uid), "%u", HEADER_DATA(hdr)->uid);
  mutt_buffer_clear (cmd);
  mutt_buffer_addstr (cmd, "UID STORE ");
  mutt_buffer_addstr (cmd, uid);

  flags[0] = '\0';

  imap_set_flag (idata, MUTT_ACL_SEEN, hdr->read, "\\Seen ",
		 flags, sizeof (flags));
  imap_set_flag (idata, MUTT_ACL_WRITE, hdr->old,
                 "Old ", flags, sizeof (flags));
  imap_set_flag (idata, MUTT_ACL_WRITE, hdr->flagged,
		 "\\Flagged ", flags, sizeof (flags));
  imap_set_flag (idata, MUTT_ACL_WRITE, hdr->replied,
		 "\\Answered ", flags, sizeof (flags));
  imap_set_flag (idata, MUTT_ACL_DELETE, HEADER_DATA(hdr)->deleted,
                 "\\Deleted ", flags, sizeof (flags));

  /* now make sure we don't lose custom tags */
  if (mutt_bit_isset (idata->ctx->rights, MUTT_ACL_WRITE))
    imap_add_keywords (flags, hdr, idata->flags, sizeof (flags));

  mutt_remove_trailing_ws (flags);

  /* UW-IMAP is OK with null flags, Cyrus isn't. The only solution is to
   * explicitly revoke all system flags (if we have permission) */
  if (!*flags)
  {
    imap_set_flag (idata, MUTT_ACL_SEEN, 1, "\\Seen ", flags, sizeof (flags));
    imap_set_flag (idata, MUTT_ACL_WRITE, 1, "Old ", flags, sizeof (flags));
    imap_set_flag (idata, MUTT_ACL_WRITE, 1, "\\Flagged ", flags, sizeof (flags));
    imap_set_flag (idata, MUTT_ACL_WRITE, 1, "\\Answered ", flags, sizeof (flags));
    imap_set_flag (idata, MUTT_ACL_DELETE, !HEADER_DATA(hdr)->deleted,
                   "\\Deleted ", flags, sizeof (flags));

    mutt_remove_trailing_ws (flags);

    mutt_buffer_addstr (cmd, " -FLAGS.SILENT (");
  }
  else
    mutt_buffer_addstr (cmd, " FLAGS.SILENT (");

  mutt_buffer_addstr (cmd, flags);
  mutt_buffer_addstr (cmd, ")");

  /* after all this it's still possible to have no flags, if you
   * have no ACL rights */
  if (*flags && (imap_exec (idata, cmd->data, 0) != 0) &&
      err_continue && (*err_continue != MUTT_YES))
  {
    *err_continue = imap_continue ("imap_sync_message: STORE failed",
				   idata->buf);
    if (*err_continue != MUTT_YES)
      return -1;
  }

  if (hdr->deleted == HEADER_DATA(hdr)->deleted)
    hdr->changed = 0;

  return 0;
}

static int sync_helper (IMAP_DATA* idata, int right, int flag, const char* name)
{
  int count = 0;
  int rc;
  char buf[LONG_STRING];

  if (!idata->ctx)
    return -1;

  if (!mutt_bit_isset (idata->ctx->rights, right))
    return 0;

  if (right == MUTT_ACL_WRITE && !imap_has_flag (idata->flags, name))
    return 0;

  snprintf (buf, sizeof(buf), "+FLAGS.SILENT (%s)", name);
  if ((rc = imap_exec_msgset (idata, "UID STORE", buf, flag, 1, 0)) < 0)
    return rc;
  count += rc;

  buf[0] = '-';
  if ((rc = imap_exec_msgset (idata, "UID STORE", buf, flag, 1, 1)) < 0)
    return rc;
  count += rc;

  return count;
}

/* update the IMAP server to reflect message changes done within mutt.
 * Arguments
 *   ctx: the current context
 *   expunge: 0 or 1 - do expunge?
 */
int imap_sync_mailbox (CONTEXT* ctx, int expunge, int* index_hint)
{
  IMAP_DATA* idata;
  CONTEXT* appendctx = NULL;
  HEADER* h;
  HEADER** hdrs = NULL;
  int oldsort;
  int n;
  int rc, quickdel_rc = 0;

  idata = (IMAP_DATA*) ctx->data;

  if (idata->state < IMAP_SELECTED)
  {
    dprint (2, (debugfile, "imap_sync_mailbox: no mailbox selected\n"));
    return -1;
  }

  /* This function is only called when the calling code expects the context
   * to be changed. */
  imap_allow_reopen (ctx);

  if ((rc = imap_check_mailbox (ctx, index_hint, 0)) != 0)
    goto out;

  /* if we are expunging anyway, we can do deleted messages very quickly... */
  if (expunge && mutt_bit_isset (ctx->rights, MUTT_ACL_DELETE))
  {
    if ((quickdel_rc = imap_exec_msgset (idata,
                                         "UID STORE", "+FLAGS.SILENT (\\Deleted)",
                                         MUTT_DELETED, 1, 0)) < 0)
    {
      rc = quickdel_rc;
      mutt_error (_("Expunge failed"));
      mutt_sleep (1);
      goto out;
    }

    if (quickdel_rc > 0)
    {
      /* mark these messages as unchanged so second pass ignores them. Done
       * here so BOGUS UW-IMAP 4.7 SILENT FLAGS updates are ignored. */
      for (n = 0; n < ctx->msgcount; n++)
        if (ctx->hdrs[n]->deleted && ctx->hdrs[n]->changed)
          ctx->hdrs[n]->active = 0;
      if (!ctx->quiet)
        mutt_message (_("Marking %d messages deleted..."), quickdel_rc);
    }
  }

#if USE_HCACHE
  idata->hcache = imap_hcache_open (idata, NULL);
#endif

  /* save messages with real (non-flag) changes */
  for (n = 0; n < ctx->msgcount; n++)
  {
    h = ctx->hdrs[n];

    if (h->deleted)
    {
      imap_cache_del (idata, h);
#if USE_HCACHE
      imap_hcache_del (idata, HEADER_DATA(h)->uid);
#endif
    }

    if (h->active && h->changed)
    {
#if USE_HCACHE
      imap_hcache_put (idata, h);
#endif
      /* if the message has been rethreaded or attachments have been deleted
       * we delete the message and reupload it.
       * This works better if we're expunging, of course. */
      /* TODO: why the h->env check? */
      if ((h->env && h->env->changed) || h->attach_del)
      {
        /* NOTE and TODO:
         *
         * The mx_open_mailbox() in append mode below merely hijacks an existing
         * idata; it doesn't reset idata->ctx.  imap_append_message() ends up
         * using (borrowing) the same idata we are using.
         *
         * Right after the APPEND operation finishes, the server can send an
         * EXISTS notifying of the new message.  Then, while still inside
         * imap_append_message(), imap_cmd_step() -> imap_cmd_finish() will
         * call imap_read_headers() to download those (because the idata's
         * reopen_allow is set).
         *
         * The imap_read_headers() will open (and clobber) the idata->hcache we
         * just opened above, then close it.
         *
         * The easy and less dangerous fix done here (for a stable branch bug
         * fix) is to close and reopen the header cache around the operation.
         *
         * A better fix would be allowing idata->hcache reuse.  When that is
         * done, the close/reopen in read_headers_condstore_qresync_updates()
         * can also be removed. */
#if USE_HCACHE
        imap_hcache_close (idata);
#endif
        if (!ctx->quiet)
          mutt_message (_("Saving changed messages... [%d/%d]"), n+1,
                        ctx->msgcount);
	if (!appendctx)
	  appendctx = mx_open_mailbox (ctx->path, MUTT_APPEND | MUTT_QUIET, NULL);
	if (!appendctx)
	  dprint (1, (debugfile, "imap_sync_mailbox: Error opening mailbox in append mode\n"));
	else
	  _mutt_save_message (h, appendctx, 1, 0, 0);
        /* TODO: why the check for h->env?  Is this possible? */
        if (h->env)
          h->env->changed = 0;
#if USE_HCACHE
        idata->hcache = imap_hcache_open (idata, NULL);
#endif
      }
    }
  }

#if USE_HCACHE
  imap_hcache_close (idata);
#endif

  /* presort here to avoid doing 10 resorts in imap_exec_msgset.
   *
   * Note: sync_helper() may trigger an imap_exec() if the queue fills
   * up.  Because IMAP_REOPEN_ALLOW is set, this may result in new
   * messages being downloaded or an expunge being processed.  For new
   * messages this would both result in memory corruption (since we're
   * alloc'ing msgcount instead of hdrmax pointers) and data loss of
   * the new messages.  For an expunge, the restored hdrs would point
   * to headers that have been freed.
   *
   * Since reopen is allowed, we could sort before and after but this
   * is noticable slower.
   *
   * So instead, just turn off reopen_allow for the duration of the
   * swapped hdrs.  The imap_exec() below flushes the queue out,
   * giving the opportunity to process any reopen events.
   */
  imap_disallow_reopen (ctx);
  oldsort = Sort;
  if (Sort != SORT_UID)
  {
    hdrs = ctx->hdrs;
    ctx->hdrs = safe_malloc (ctx->msgcount * sizeof (HEADER*));
    memcpy (ctx->hdrs, hdrs, ctx->msgcount * sizeof (HEADER*));

    Sort = SORT_UID;
    qsort (ctx->hdrs, ctx->msgcount, sizeof (HEADER*),
           compare_uid);
  }

  rc = sync_helper (idata, MUTT_ACL_DELETE, MUTT_DELETED, "\\Deleted");
  if (rc >= 0)
    rc |= sync_helper (idata, MUTT_ACL_WRITE, MUTT_FLAG, "\\Flagged");
  if (rc >= 0)
    rc |= sync_helper (idata, MUTT_ACL_WRITE, MUTT_OLD, "Old");
  if (rc >= 0)
    rc |= sync_helper (idata, MUTT_ACL_SEEN, MUTT_READ, "\\Seen");
  if (rc >= 0)
    rc |= sync_helper (idata, MUTT_ACL_WRITE, MUTT_REPLIED, "\\Answered");

  if ((oldsort != Sort) || hdrs)
  {
    Sort = oldsort;
    FREE (&ctx->hdrs);
    ctx->hdrs = hdrs;
  }
  imap_allow_reopen (ctx);

  /* Flush the queued flags if any were changed in sync_helper.
   * The real (non-flag) changes loop might have flushed quickdel_rc
   * queued commands, so we double check the cmdbuf isn't empty. */
  if (((rc > 0) || (quickdel_rc > 0)) && mutt_buffer_len (idata->cmdbuf))
    if (imap_exec (idata, NULL, 0) != IMAP_CMD_OK)
      rc = -1;

  if (rc < 0)
  {
    if (ctx->closing)
    {
      if (mutt_yesorno (_("Error saving flags. Close anyway?"), 0) == MUTT_YES)
      {
        rc = 0;
        idata->state = IMAP_AUTHENTICATED;
        goto out;
      }
    }
    else
      mutt_error _("Error saving flags");
    rc = -1;
    goto out;
  }

  /* Update local record of server state to reflect the synchronization just
   * completed.  imap_read_headers always overwrites hcache-origin flags, so
   * there is no need to mutate the hcache after flag-only changes. */
  for (n = 0; n < ctx->msgcount; n++)
  {
    HEADER_DATA(ctx->hdrs[n])->deleted = ctx->hdrs[n]->deleted;
    HEADER_DATA(ctx->hdrs[n])->flagged = ctx->hdrs[n]->flagged;
    HEADER_DATA(ctx->hdrs[n])->old = ctx->hdrs[n]->old;
    HEADER_DATA(ctx->hdrs[n])->read = ctx->hdrs[n]->read;
    HEADER_DATA(ctx->hdrs[n])->replied = ctx->hdrs[n]->replied;
    ctx->hdrs[n]->changed = 0;
  }
  ctx->changed = 0;

  /* We must send an EXPUNGE command if we're not closing. */
  if (expunge && !(ctx->closing) &&
      mutt_bit_isset(ctx->rights, MUTT_ACL_DELETE))
  {
    if (!ctx->quiet)
      mutt_message _("Expunging messages from server...");
    /* Set expunge bit so we don't get spurious reopened messages */
    idata->reopen |= IMAP_EXPUNGE_EXPECTED;
    if (imap_exec (idata, "EXPUNGE", 0) != 0)
    {
      idata->reopen &= ~IMAP_EXPUNGE_EXPECTED;
      imap_error (_("imap_sync_mailbox: EXPUNGE failed"), idata->buf);
      rc = -1;
      goto out;
    }
    idata->reopen &= ~IMAP_EXPUNGE_EXPECTED;
  }

  if (expunge && ctx->closing)
  {
    imap_exec (idata, "CLOSE", 0);
    idata->state = IMAP_AUTHENTICATED;
  }

  if (option (OPTMESSAGECACHECLEAN))
    imap_cache_clean (idata);

  rc = 0;

out:
  imap_disallow_reopen (ctx);
  if (appendctx)
  {
    mx_fastclose_mailbox (appendctx);
    FREE (&appendctx);
  }
  return rc;
}

/* imap_close_mailbox: clean up IMAP data in CONTEXT */
int imap_close_mailbox (CONTEXT* ctx)
{
  IMAP_DATA* idata;
  int i;

  idata = (IMAP_DATA*) ctx->data;
  /* Check to see if the mailbox is actually open */
  if (!idata)
    return 0;

  /* imap_open_mailbox_append() borrows the IMAP_DATA temporarily,
   * just for the connection, but does not set idata->ctx to the
   * open-append ctx.
   *
   * So when these are equal, it means we are actually closing the
   * mailbox and should clean up idata.  Otherwise, we don't want to
   * touch idata - it's still being used.
   */
  if (ctx == idata->ctx)
  {
    if (idata->status != IMAP_FATAL && idata->state >= IMAP_SELECTED)
    {
      /* mx_close_mailbox won't sync if there are no deleted messages
       * and the mailbox is unchanged, so we may have to close here */
      if (!ctx->deleted)
        imap_exec (idata, "CLOSE", 0);
      idata->state = IMAP_AUTHENTICATED;
    }

    idata->check_status = 0;
    idata->reopen = 0;
    FREE (&(idata->mailbox));
    mutt_free_list (&idata->flags);
    idata->ctx = NULL;

    hash_destroy (&idata->uid_hash, NULL);
    FREE (&idata->msn_index);
    idata->msn_index_size = 0;
    idata->max_msn = 0;

    for (i = 0; i < IMAP_CACHE_LEN; i++)
    {
      if (idata->cache[i].path)
      {
        unlink (idata->cache[i].path);
        FREE (&idata->cache[i].path);
      }
    }

    mutt_bcache_close (&idata->bcache);
  }

  /* free IMAP part of headers */
  for (i = 0; i < ctx->msgcount; i++)
    /* mailbox may not have fully loaded */
    if (ctx->hdrs[i] && ctx->hdrs[i]->data)
      imap_free_header_data ((IMAP_HEADER_DATA**)&(ctx->hdrs[i]->data));

  return 0;
}

/* use the NOOP or IDLE command to poll for new mail
 *
 * return values:
 *	MUTT_REOPENED	mailbox has been externally modified
 *	MUTT_NEW_MAIL	new mail has arrived!
 *	0		no change
 *	-1		error
 */
int imap_check_mailbox (CONTEXT *ctx, int *index_hint, int force)
{
  /* overload keyboard timeout to avoid many mailbox checks in a row.
   * Most users don't like having to wait exactly when they press a key. */
  IMAP_DATA* idata;
  int result = -1, poll_rc;

  idata = (IMAP_DATA*) ctx->data;

  /* try IDLE first, unless force is set */
  if (!force && option (OPTIMAPIDLE) && mutt_bit_isset (idata->capabilities, IDLE)
      && (idata->state != IMAP_IDLE || time(NULL) >= idata->lastread + ImapKeepalive))
  {
    if (imap_cmd_idle (idata) < 0)
      goto errcleanup;
  }
  if (idata->state == IMAP_IDLE)
  {
    while ((poll_rc = mutt_socket_poll (idata->conn, 0)) > 0)
    {
      if (imap_cmd_step (idata) != IMAP_CMD_CONTINUE)
      {
        dprint (1, (debugfile, "Error reading IDLE response\n"));
        goto errcleanup;
      }
    }
    if (poll_rc < 0)
    {
      dprint (1, (debugfile, "Poll failed, disabling IDLE\n"));
      mutt_bit_unset (idata->capabilities, IDLE);
    }
  }

  if ((force ||
       (idata->state != IMAP_IDLE && time(NULL) >= idata->lastread + Timeout))
      && imap_exec (idata, "NOOP", IMAP_CMD_POLL) != 0)
    goto errcleanup;

  /* We call this even when we haven't run NOOP in case we have pending
   * changes to process, since we can reopen here. */
  imap_cmd_finish (idata);

  result = 0;

errcleanup:
  /* Try to reconnect Context if a cmd_handle_fatal() was flagged */
  if (idata->status == IMAP_FATAL)
  {
    if ((idata->reopen & IMAP_REOPEN_ALLOW) &&
        Context &&
        idata->ctx == Context)
    {
      if (imap_reconnect (&idata) == 0)
      {
        idata->check_status = 0;
        return MUTT_RECONNECTED;
      }
    }
    return -1;
  }

  if (idata->check_status & IMAP_EXPUNGE_PENDING)
    result = MUTT_REOPENED;
  else if (idata->check_status & IMAP_NEWMAIL_PENDING)
    result = MUTT_NEW_MAIL;
  else if (idata->check_status & IMAP_FLAGS_PENDING)
    result = MUTT_FLAGS;

  idata->check_status = 0;

  return result;
}

static int imap_check_mailbox_reopen (CONTEXT *ctx, int *index_hint)
{
  int rc;

  imap_allow_reopen (ctx);
  rc = imap_check_mailbox (ctx, index_hint, 0);
  imap_disallow_reopen (ctx);

  return rc;
}

static int imap_save_to_header_cache (CONTEXT *ctx, HEADER *h)
{
  int rc = 0;
#ifdef USE_HCACHE
  int close_hc = 1;
  IMAP_DATA* idata;

  idata = (IMAP_DATA *)ctx->data;
  if (idata->hcache)
    close_hc = 0;
  else
    idata->hcache = imap_hcache_open (idata, NULL);
  rc = imap_hcache_put (idata, h);
  if (close_hc)
    imap_hcache_close (idata);
#endif
  return rc;
}

/* split path into (idata,mailbox name) */
static int imap_get_mailbox (const char* path, IMAP_DATA** hidata, char* buf, size_t blen)
{
  IMAP_MBOX mx;

  if (imap_parse_path (path, &mx))
  {
    dprint (1, (debugfile, "imap_get_mailbox: Error parsing %s\n", path));
    return -1;
  }
  if (!(*hidata = imap_conn_find (&(mx.account), option (OPTIMAPPASSIVE) ? MUTT_IMAP_CONN_NONEW : 0)))
  {
    FREE (&mx.mbox);
    return -1;
  }

  imap_fix_path (*hidata, mx.mbox, buf, blen);
  if (!*buf)
    strfcpy (buf, "INBOX", blen);
  FREE (&mx.mbox);

  return 0;
}

/* check for new mail in any subscribed mailboxes. Given a list of mailboxes
 * rather than called once for each so that it can batch the commands and
 * save on round trips. Returns number of mailboxes with new mail. */
int imap_buffy_check (int force, int check_stats)
{
  IMAP_DATA* idata;
  IMAP_DATA* lastdata = NULL;
  BUFFY* mailbox;
  char name[LONG_STRING];
  char command[LONG_STRING*2];
  char munged[LONG_STRING];
  int buffies = 0;

  for (mailbox = Incoming; mailbox; mailbox = mailbox->next)
  {
    /* Init newly-added mailboxes */
    if (! mailbox->magic)
    {
      if (mx_is_imap (mutt_b2s (mailbox->pathbuf)))
        mailbox->magic = MUTT_IMAP;
    }

    if (mailbox->magic != MUTT_IMAP)
      continue;

    if (mailbox->nopoll)
      continue;

    if (imap_get_mailbox (mutt_b2s (mailbox->pathbuf), &idata, name, sizeof (name)) < 0)
    {
      mailbox->new = 0;
      continue;
    }

    /* Don't issue STATUS on the selected mailbox, it will be NOOPed or
     * IDLEd elsewhere.
     * idata->mailbox may be NULL for connections other than the current
     * mailbox's, and shouldn't expand to INBOX in that case. #3216. */
    if (idata->mailbox && !imap_mxcmp (name, idata->mailbox))
    {
      mailbox->new = 0;
      continue;
    }

    if (!mutt_bit_isset (idata->capabilities, IMAP4REV1) &&
        !mutt_bit_isset (idata->capabilities, STATUS))
    {
      dprint (2, (debugfile, "Server doesn't support STATUS\n"));
      continue;
    }

    if (lastdata && idata != lastdata)
    {
      /* Send commands to previous server. Sorting the buffy list
       * may prevent some infelicitous interleavings */
      if (imap_exec (lastdata, NULL, IMAP_CMD_FAIL_OK | IMAP_CMD_POLL) == -1)
        dprint (1, (debugfile, "Error polling mailboxes\n"));

      lastdata = NULL;
    }

    if (!lastdata)
      lastdata = idata;

    imap_munge_mbox_name (idata, munged, sizeof (munged), name);
    if (check_stats)
      snprintf (command, sizeof (command),
                "STATUS %s (UIDNEXT UIDVALIDITY UNSEEN RECENT MESSAGES)", munged);
    else
      snprintf (command, sizeof (command),
                "STATUS %s (UIDNEXT UIDVALIDITY UNSEEN RECENT)", munged);

    if (imap_exec (idata, command, IMAP_CMD_QUEUE | IMAP_CMD_POLL) < 0)
    {
      dprint (1, (debugfile, "Error queueing command\n"));
      return 0;
    }
  }

  if (lastdata && (imap_exec (lastdata, NULL, IMAP_CMD_FAIL_OK | IMAP_CMD_POLL) == -1))
  {
    dprint (1, (debugfile, "Error polling mailboxes\n"));
    return 0;
  }

  /* collect results */
  for (mailbox = Incoming; mailbox; mailbox = mailbox->next)
  {
    if (mailbox->magic == MUTT_IMAP && mailbox->new)
      buffies++;
  }

  return buffies;
}

/* imap_status: returns count of messages in mailbox, or -1 on error.
 * if queue != 0, queue the command and expect it to have been run
 * on the next call (for pipelining the postponed count) */
int imap_status (const char* path, int queue)
{
  static int queued = 0;

  IMAP_DATA *idata;
  char buf[LONG_STRING*2];
  char mbox[LONG_STRING];
  IMAP_STATUS* status;

  if (imap_get_mailbox (path, &idata, buf, sizeof (buf)) < 0)
    return -1;

  /* We are in the folder we're polling - just return the mailbox count.
   *
   * Note that imap_mxcmp() converts NULL to "INBOX", so we need to
   * make sure the idata really is open to a folder. */
  if (idata->ctx && !imap_mxcmp (buf, idata->mailbox))
    return idata->ctx->msgcount;
  else if (mutt_bit_isset(idata->capabilities,IMAP4REV1) ||
	   mutt_bit_isset(idata->capabilities,STATUS))
  {
    imap_munge_mbox_name (idata, mbox, sizeof(mbox), buf);
    snprintf (buf, sizeof (buf), "STATUS %s (%s)", mbox, "MESSAGES");
    imap_unmunge_mbox_name (idata, mbox);
  }
  else
    /* Server does not support STATUS, and this is not the current mailbox.
     * There is no lightweight way to check recent arrivals */
    return -1;

  if (queue)
  {
    imap_exec (idata, buf, IMAP_CMD_QUEUE);
    queued = 1;
    return 0;
  }
  else if (!queued)
    imap_exec (idata, buf, 0);

  queued = 0;
  if ((status = imap_mboxcache_get (idata, mbox, 0)))
    return status->messages;

  return 0;
}

/* return cached mailbox stats or NULL if create is 0 */
IMAP_STATUS* imap_mboxcache_get (IMAP_DATA* idata, const char* mbox, int create)
{
  LIST* cur;
  IMAP_STATUS* status;
  IMAP_STATUS scache;
#ifdef USE_HCACHE
  header_cache_t *hc = NULL;
  void *puidvalidity = NULL;
  void *puidnext = NULL;
  void *pmodseq = NULL;
#endif

  for (cur = idata->mboxcache; cur; cur = cur->next)
  {
    status = (IMAP_STATUS*)cur->data;

    if (!imap_mxcmp (mbox, status->name))
      return status;
  }
  status = NULL;

  /* lame */
  if (create)
  {
    memset (&scache, 0, sizeof (scache));
    scache.name = (char*)mbox;
    idata->mboxcache = mutt_add_list_n (idata->mboxcache, &scache,
                                        sizeof (scache));
    status = imap_mboxcache_get (idata, mbox, 0);
    status->name = safe_strdup (mbox);
  }

#ifdef USE_HCACHE
  hc = imap_hcache_open (idata, mbox);
  if (hc)
  {
    puidvalidity = mutt_hcache_fetch_raw (hc, "/UIDVALIDITY", imap_hcache_keylen);
    puidnext = mutt_hcache_fetch_raw (hc, "/UIDNEXT", imap_hcache_keylen);
    pmodseq = mutt_hcache_fetch_raw (hc, "/MODSEQ", imap_hcache_keylen);
    if (puidvalidity)
    {
      if (!status)
      {
        mutt_hcache_free ((void **)&puidvalidity);
        mutt_hcache_free ((void **)&puidnext);
        mutt_hcache_free ((void **)&pmodseq);
        mutt_hcache_close (hc);
        return imap_mboxcache_get (idata, mbox, 1);
      }
      memcpy (&status->uidvalidity, puidvalidity, sizeof(unsigned int));

      if (puidnext)
        memcpy (&status->uidnext, puidnext, sizeof(unsigned int));
      else
        status->uidnext = 0;

      if (pmodseq)
        memcpy (&status->modseq, pmodseq, sizeof(unsigned long long));
      else
        status->modseq = 0;
      dprint (3, (debugfile, "mboxcache: hcache uidvalidity %u, uidnext %u, modseq %llu\n",
                  status->uidvalidity, status->uidnext, status->modseq));
    }
    mutt_hcache_free ((void **)&puidvalidity);
    mutt_hcache_free ((void **)&puidnext);
    mutt_hcache_free ((void **)&pmodseq);
    mutt_hcache_close (hc);
  }
#endif

  return status;
}

void imap_mboxcache_free (IMAP_DATA* idata)
{
  LIST* cur;
  IMAP_STATUS* status;

  for (cur = idata->mboxcache; cur; cur = cur->next)
  {
    status = (IMAP_STATUS*)cur->data;

    FREE (&status->name);
  }

  mutt_free_list (&idata->mboxcache);
}

/* returns number of patterns in the search that should be done server-side
 * (eg are full-text) */
static int do_search (const pattern_t* search, int allpats)
{
  int rc = 0;
  const pattern_t* pat;

  for (pat = search; pat; pat = pat->next)
  {
    switch (pat->op)
    {
      case MUTT_BODY:
      case MUTT_HEADER:
      case MUTT_WHOLE_MSG:
        if (pat->stringmatch)
          rc++;
        break;
      default:
        if (pat->child && do_search (pat->child, 1))
          rc++;
    }

    if (!allpats)
      break;
  }

  return rc;
}

/* convert mutt pattern_t to IMAP SEARCH command containing only elements
 * that require full-text search (mutt already has what it needs for most
 * match types, and does a better job (eg server doesn't support regexps). */
static int imap_compile_search (const pattern_t* pat, BUFFER* buf)
{
  if (! do_search (pat, 0))
    return 0;

  if (pat->not)
    mutt_buffer_addstr (buf, "NOT ");

  if (pat->child)
  {
    int clauses;

    if ((clauses = do_search (pat->child, 1)) > 0)
    {
      const pattern_t* clause = pat->child;

      mutt_buffer_addch (buf, '(');

      while (clauses)
      {
        if (do_search (clause, 0))
        {
          if (pat->op == MUTT_OR && clauses > 1)
            mutt_buffer_addstr (buf, "OR ");
          clauses--;

          if (imap_compile_search (clause, buf) < 0)
            return -1;

          if (clauses)
            mutt_buffer_addch (buf, ' ');

        }
        clause = clause->next;
      }

      mutt_buffer_addch (buf, ')');
    }
  }
  else
  {
    char term[STRING];
    char *delim;

    switch (pat->op)
    {
      case MUTT_HEADER:
        mutt_buffer_addstr (buf, "HEADER ");

        /* extract header name */
        if (! (delim = strchr (pat->p.str, ':')))
        {
          mutt_error (_("Header search without header name: %s"), pat->p.str);
          return -1;
        }
        *delim = '\0';
        imap_quote_string (term, sizeof (term), pat->p.str);
        mutt_buffer_addstr (buf, term);
        mutt_buffer_addch (buf, ' ');

        /* and field */
        *delim = ':';
        delim++;
        SKIPWS(delim);
        imap_quote_string (term, sizeof (term), delim);
        mutt_buffer_addstr (buf, term);
        break;
      case MUTT_BODY:
        mutt_buffer_addstr (buf, "BODY ");
        imap_quote_string (term, sizeof (term), pat->p.str);
        mutt_buffer_addstr (buf, term);
        break;
      case MUTT_WHOLE_MSG:
        mutt_buffer_addstr (buf, "TEXT ");
        imap_quote_string (term, sizeof (term), pat->p.str);
        mutt_buffer_addstr (buf, term);
        break;
    }
  }

  return 0;
}

int imap_search (CONTEXT* ctx, const pattern_t* pat)
{
  BUFFER buf;
  IMAP_DATA* idata = (IMAP_DATA*)ctx->data;
  int i;

  for (i = 0; i < ctx->msgcount; i++)
    ctx->hdrs[i]->matched = 0;

  if (!do_search (pat, 1))
    return 0;

  mutt_buffer_init (&buf);
  mutt_buffer_addstr (&buf, "UID SEARCH ");
  if (imap_compile_search (pat, &buf) < 0)
  {
    FREE (&buf.data);
    return -1;
  }
  if (imap_exec (idata, buf.data, 0) < 0)
  {
    FREE (&buf.data);
    return -1;
  }

  FREE (&buf.data);
  return 0;
}

int imap_subscribe (char *path, int subscribe)
{
  IMAP_DATA *idata;
  char buf[LONG_STRING*2];
  char mbox[LONG_STRING];
  IMAP_MBOX mx;

  if (!mx_is_imap (path) || imap_parse_path (path, &mx) || !mx.mbox)
  {
    mutt_error (_("Bad mailbox name"));
    return -1;
  }
  if (!(idata = imap_conn_find (&(mx.account), 0)))
    goto fail;

  imap_fix_path (idata, mx.mbox, buf, sizeof (buf));
  if (!*buf)
    strfcpy (buf, "INBOX", sizeof (buf));

  if (option (OPTIMAPCHECKSUBSCRIBED))
  {
    if (subscribe)
      mutt_buffy_add (path, NULL, -1, -1);
    else
      mutt_buffy_remove (path);
  }

  if (subscribe)
    mutt_message (_("Subscribing to %s..."), buf);
  else
    mutt_message (_("Unsubscribing from %s..."), buf);
  imap_munge_mbox_name (idata, mbox, sizeof(mbox), buf);

  snprintf (buf, sizeof (buf), "%sSUBSCRIBE %s", subscribe ? "" : "UN", mbox);

  if (imap_exec (idata, buf, 0) < 0)
    goto fail;

  imap_unmunge_mbox_name(idata, mx.mbox);
  if (subscribe)
    mutt_message (_("Subscribed to %s"), mx.mbox);
  else
    mutt_message (_("Unsubscribed from %s"), mx.mbox);
  FREE (&mx.mbox);
  return 0;

fail:
  FREE (&mx.mbox);
  return -1;
}

/* trim dest to the length of the longest prefix it shares with src,
 * returning the length of the trimmed string */
static size_t
longest_common_prefix (char *dest, const char* src, size_t start, size_t dlen)
{
  size_t pos = start;

  while (pos < dlen && dest[pos] && dest[pos] == src[pos])
    pos++;
  dest[pos] = '\0';

  return pos;
}

/* look for IMAP URLs to complete from defined mailboxes. Could be extended
 * to complete over open connections and account/folder hooks too. */
static int
imap_complete_hosts (char *dest, size_t len)
{
  BUFFY* mailbox;
  CONNECTION* conn;
  int rc = -1;
  size_t matchlen;

  matchlen = mutt_strlen (dest);
  for (mailbox = Incoming; mailbox; mailbox = mailbox->next)
  {
    if (!mutt_strncmp (dest, mutt_b2s (mailbox->pathbuf), matchlen))
    {
      if (rc)
      {
        strfcpy (dest, mutt_b2s (mailbox->pathbuf), len);
        rc = 0;
      }
      else
        longest_common_prefix (dest, mutt_b2s (mailbox->pathbuf), matchlen, len);
    }
  }

  for (conn = mutt_socket_head (); conn; conn = conn->next)
  {
    ciss_url_t url;
    char urlstr[LONG_STRING];

    if (conn->account.type != MUTT_ACCT_TYPE_IMAP)
      continue;

    mutt_account_tourl (&conn->account, &url, 0);
    /* FIXME: how to handle multiple users on the same host? */
    url.user = NULL;
    url.path = NULL;
    url_ciss_tostring (&url, urlstr, sizeof (urlstr), 0);
    if (!mutt_strncmp (dest, urlstr, matchlen))
    {
      if (rc)
      {
        strfcpy (dest, urlstr, len);
        rc = 0;
      }
      else
        longest_common_prefix (dest, urlstr, matchlen, len);
    }
  }

  return rc;
}

/* imap_complete: given a partial IMAP folder path, return a string which
 *   adds as much to the path as is unique */
int imap_complete(char* dest, size_t dlen, const char* path)
{
  IMAP_DATA* idata;
  char list[LONG_STRING];
  char buf[LONG_STRING*2];
  IMAP_LIST listresp;
  char completion[LONG_STRING];
  size_t clen;
  size_t matchlen = 0;
  int completions = 0;
  IMAP_MBOX mx;
  int rc;

  if (imap_parse_path (path, &mx))
  {
    strfcpy (dest, path, dlen);
    return imap_complete_hosts (dest, dlen);
  }

  /* don't open a new socket just for completion. Instead complete over
   * known mailboxes/hooks/etc */
  if (!(idata = imap_conn_find (&(mx.account), MUTT_IMAP_CONN_NONEW)))
  {
    FREE (&mx.mbox);
    strfcpy (dest, path, dlen);
    return imap_complete_hosts (dest, dlen);
  }

  /* reformat path for IMAP list, and append wildcard */
  /* don't use INBOX in place of "" */
  if (mx.mbox && mx.mbox[0])
    imap_fix_path (idata, mx.mbox, list, sizeof(list));
  else
    list[0] = '\0';

  /* fire off command */
  snprintf (buf, sizeof(buf), "%s \"\" \"%s%%\"",
            option (OPTIMAPLSUB) ? "LSUB" : "LIST", list);

  imap_cmd_start (idata, buf);

  /* and see what the results are */
  strfcpy (completion, NONULL(mx.mbox), sizeof(completion));
  idata->cmdtype = IMAP_CT_LIST;
  idata->cmddata = &listresp;
  do
  {
    listresp.name = NULL;
    rc = imap_cmd_step (idata);

    if (rc == IMAP_CMD_CONTINUE && listresp.name)
    {
      /* if the folder isn't selectable, append delimiter to force browse
       * to enter it on second tab. */
      if (listresp.noselect)
      {
        clen = strlen(listresp.name);
        listresp.name[clen++] = listresp.delim;
        listresp.name[clen] = '\0';
      }
      /* copy in first word */
      if (!completions)
      {
        strfcpy (completion, listresp.name, sizeof(completion));
        matchlen = strlen (completion);
        completions++;
        continue;
      }

      matchlen = longest_common_prefix (completion, listresp.name, 0, matchlen);
      completions++;
    }
  }
  while (rc == IMAP_CMD_CONTINUE);
  idata->cmddata = NULL;

  if (completions)
  {
    /* reformat output */
    imap_qualify_path (dest, dlen, &mx, completion);
    mutt_pretty_mailbox (dest, dlen);

    FREE (&mx.mbox);
    return 0;
  }

  FREE (&mx.mbox);
  return -1;
}

/* imap_fast_trash: use server COPY command to copy deleted
 * messages to the trash folder.
 *   Return codes:
 *      -1: error
 *       0: success
 *       1: non-fatal error - try fetch/append */
int imap_fast_trash (CONTEXT* ctx, char* dest)
{
  IMAP_DATA* idata;
  char mbox[LONG_STRING];
  char mmbox[LONG_STRING];
  char prompt[LONG_STRING];
  int n, rc;
  IMAP_MBOX mx;
  int triedcreate = 0;
  BUFFER *sync_cmd = NULL;
  int err_continue = MUTT_NO;

  idata = (IMAP_DATA*) ctx->data;

  if (imap_parse_path (dest, &mx))
  {
    dprint (1, (debugfile, "imap_fast_trash: bad destination %s\n", dest));
    return -1;
  }

  /* check that the save-to folder is in the same account */
  if (!mutt_account_match (&(CTX_DATA->conn->account), &(mx.account)))
  {
    dprint (3, (debugfile, "imap_fast_trash: %s not same server as %s\n",
                dest, ctx->path));
    FREE (&mx.mbox);
    return 1;
  }

  /* Scan if any of the messages were previously checkpoint-deleted
   * on the server, by answering "no" to $delete for instance.
   * In that case, doing a UID COPY would also copy the deleted flag, which
   * is probably not desired.  Trying to work around that leads to all sorts
   * of headaches, so just force a manual append.
   */
  for (n = 0; n < ctx->msgcount; n++)
  {
    if (ctx->hdrs[n]->active &&
        ctx->hdrs[n]->deleted && !ctx->hdrs[n]->purge &&
        HEADER_DATA(ctx->hdrs[n])->deleted)
    {
      dprint (1, (debugfile,
                  "imap_fast_trash: server-side delete flag set. aborting.\n"));
      rc = -1;
      goto out;
    }
  }

  imap_fix_path (idata, mx.mbox, mbox, sizeof (mbox));
  if (!*mbox)
    strfcpy (mbox, "INBOX", sizeof (mbox));
  imap_munge_mbox_name (idata, mmbox, sizeof (mmbox), mbox);

  sync_cmd = mutt_buffer_new ();
  for (n = 0; n < ctx->msgcount; n++)
  {
    if (ctx->hdrs[n]->active && ctx->hdrs[n]->changed &&
        ctx->hdrs[n]->deleted && !ctx->hdrs[n]->purge)
    {
      rc = imap_sync_message_for_copy (idata, ctx->hdrs[n], sync_cmd, &err_continue);
      if (rc < 0)
      {
        dprint (1, (debugfile, "imap_fast_trash: could not sync\n"));
        goto out;
      }
    }
  }

  /* loop in case of TRYCREATE */
  do
  {
    rc = imap_exec_msgset (idata, "UID COPY", mmbox, MUTT_TRASH, 0, 0);
    if (!rc)
    {
      dprint (1, (debugfile, "imap_fast_trash: No messages to trash\n"));
      rc = -1;
      goto out;
    }
    else if (rc < 0)
    {
      dprint (1, (debugfile, "could not queue copy\n"));
      goto out;
    }
    else if (!ctx->quiet)
      mutt_message (_("Copying %d messages to %s..."), rc, mbox);

    /* let's get it on */
    rc = imap_exec (idata, NULL, IMAP_CMD_FAIL_OK);
    if (rc == -2)
    {
      if (triedcreate)
      {
        dprint (1, (debugfile, "Already tried to create mailbox %s\n", mbox));
        break;
      }
      /* bail out if command failed for reasons other than nonexistent target */
      if (ascii_strncasecmp (imap_get_qualifier (idata->buf), "[TRYCREATE]", 11))
        break;
      dprint (3, (debugfile, "imap_fast_trash: server suggests TRYCREATE\n"));
      snprintf (prompt, sizeof (prompt), _("Create %s?"), mbox);
      if (option (OPTCONFIRMCREATE) &&
          mutt_query_boolean (OPTCONFIRMCREATE, prompt, 1) < 1)
      {
        mutt_clear_error ();
        goto out;
      }
      if (imap_create_mailbox (idata, mbox) < 0)
        break;
      triedcreate = 1;
    }
  }
  while (rc == -2);

  if (rc != 0)
  {
    imap_error ("imap_fast_trash", idata->buf);
    goto out;
  }

  rc = 0;

out:
  mutt_buffer_free (&sync_cmd);
  FREE (&mx.mbox);

  return rc < 0 ? -1 : rc;
}

struct mx_ops mx_imap_ops = {
  .open = imap_open_mailbox,
  .open_append = imap_open_mailbox_append,
  .close = imap_close_mailbox,
  .open_msg = imap_fetch_message,
  .close_msg = imap_close_message,
  .commit_msg = imap_commit_message,
  .open_new_msg = imap_open_new_message,
  .check = imap_check_mailbox_reopen,
  .sync = NULL,      /* imap syncing is handled by imap_sync_mailbox */
  .save_to_header_cache = imap_save_to_header_cache,
};
