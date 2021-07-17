/*
 * Copyright (C) 1996-2009,2012 Michael R. Elkins <me@mutt.org>
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
#include "mutt_crypt.h"
#include "mutt_idna.h"
#include "background.h"

#include <sys/stat.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

/* Returns 0 on normal exit
 *        -1 on error
 *         2 if edit headers is backgrounded.
 */
int mutt_edit_headers (const char *editor,
                       SEND_CONTEXT *sctx, int flags)
{
  const char *filename;
  FILE *ifp, *ofp;
  struct stat st;
  int rc = -1;

  filename = sctx->msg->content->filename;

  if (flags != MUTT_EDIT_HEADERS_RESUME)
  {
    sctx->tempfile = mutt_buffer_new ();
    mutt_buffer_mktemp (sctx->tempfile);
    if ((ofp = safe_fopen (mutt_b2s (sctx->tempfile), "w")) == NULL)
    {
      mutt_perror (mutt_b2s (sctx->tempfile));
      goto cleanup;
    }

    mutt_env_to_local (sctx->msg->env);
    mutt_write_rfc822_header (ofp, sctx->msg->env, NULL, NULL, MUTT_WRITE_HEADER_EDITHDRS, 0, 0);
    fputc ('\n', ofp);	/* tie off the header. */

    /* now copy the body of the message. */
    if ((ifp = fopen (filename, "r")) == NULL)
    {
      mutt_perror (filename);
      goto cleanup;
    }

    mutt_copy_stream (ifp, ofp);

    safe_fclose (&ifp);
    safe_fclose (&ofp);

    if (stat (mutt_b2s (sctx->tempfile), &st) == -1)
    {
      mutt_perror (mutt_b2s (sctx->tempfile));
      goto cleanup;
    }

    sctx->tempfile_mtime = mutt_decrease_mtime (mutt_b2s (sctx->tempfile), &st);
    if (sctx->tempfile_mtime == (time_t) -1)
    {
      mutt_perror (mutt_b2s (sctx->tempfile));
      goto cleanup;
    }

    if (flags == MUTT_EDIT_HEADERS_BACKGROUND)
    {
      if (mutt_background_edit_file (sctx, editor, mutt_b2s (sctx->tempfile)) == 2)
      {
        sctx->state = SEND_STATE_FIRST_EDIT_HEADERS;
        return 2;
      }
      flags = 0; /* fall through on error */
    }
    else
      mutt_edit_file (editor, mutt_b2s (sctx->tempfile));
  }

  if (flags != MUTT_EDIT_HEADERS_BACKGROUND)
  {
    char buffer[LONG_STRING];
    const char *p;
    int i, keep;
    ENVELOPE *n;
    LIST *cur, **last = NULL, *tmp;

    stat (mutt_b2s (sctx->tempfile), &st);
    if (sctx->tempfile_mtime == st.st_mtime)
    {
      dprint (1, (debugfile, "ci_edit_headers(): temp file was not modified.\n"));
      /* the file has not changed! */
      mutt_unlink (mutt_b2s (sctx->tempfile));
      goto cleanup;
    }

    mutt_unlink (filename);
    mutt_free_list (&sctx->msg->env->userhdrs);

    /* Read the temp file back in */
    if ((ifp = fopen (mutt_b2s (sctx->tempfile), "r")) == NULL)
    {
      mutt_perror (mutt_b2s (sctx->tempfile));
      goto cleanup;
    }

    if ((ofp = safe_fopen (filename, "w")) == NULL)
    {
      /* intentionally leak a possible temporary file here */
      safe_fclose (&ifp);
      mutt_perror (filename);
      goto cleanup;
    }

    n = mutt_read_rfc822_header (ifp, NULL, 1, 0);
    while ((i = fread (buffer, 1, sizeof (buffer), ifp)) > 0)
      fwrite (buffer, 1, i, ofp);
    safe_fclose (&ofp);
    safe_fclose (&ifp);
    mutt_unlink (mutt_b2s (sctx->tempfile));

    /* in case the user modifies/removes the In-Reply-To header with
       $edit_headers set, we remove References: as they're likely invalid;
       we can simply compare strings as we don't generate References for
       multiple Message-Ids in IRT anyways */
    if (sctx->msg->env->in_reply_to &&
        (!n->in_reply_to || mutt_strcmp (n->in_reply_to->data,
                                         sctx->msg->env->in_reply_to->data) != 0))
      mutt_free_list (&sctx->msg->env->references);

    /* restore old info. */
    mutt_free_list (&n->references);
    n->references = sctx->msg->env->references;
    sctx->msg->env->references = NULL;

    mutt_free_envelope (&sctx->msg->env);
    sctx->msg->env = n; n = NULL;

    mutt_expand_aliases_env (sctx->msg->env);

    /* search through the user defined headers added to see if
     * fcc: or attach: or pgp: was specified
     */

    cur = sctx->msg->env->userhdrs;
    last = &sctx->msg->env->userhdrs;
    while (cur)
    {
      keep = 1;

      if (ascii_strncasecmp ("fcc:", cur->data, 4) == 0)
      {
        p = skip_email_wsp(cur->data + 4);
        if (*p)
        {
          mutt_buffer_strcpy (sctx->fcc, p);
          mutt_buffer_pretty_multi_mailbox (sctx->fcc, FccDelimiter);
        }
        keep = 0;
      }
      else if (ascii_strncasecmp ("attach:", cur->data, 7) == 0)
      {
        BODY *body;
        BODY *parts;

        p = skip_email_wsp(cur->data + 7);
        if (*p)
        {
          BUFFER *path = mutt_buffer_pool_get ();
          for ( ; *p && *p != ' ' && *p != '\t'; p++)
          {
            if (*p == '\\')
            {
              if (!*(p+1))
                break;
              p++;
            }
            mutt_buffer_addch (path, *p);
          }
          p = skip_email_wsp(p);

          mutt_buffer_expand_path (path);
          if ((body = mutt_make_file_attach (mutt_b2s (path))))
          {
            body->description = safe_strdup (p);
            for (parts = sctx->msg->content; parts->next; parts = parts->next) ;
            parts->next = body;
          }
          else
          {
            mutt_buffer_pretty_mailbox (path);
            mutt_error (_("%s: unable to attach file"), mutt_b2s (path));
          }
          mutt_buffer_pool_release (&path);
        }
        keep = 0;
      }
      else if ((WithCrypto & APPLICATION_PGP)
               && ascii_strncasecmp ("pgp:", cur->data, 4) == 0)
      {
        sctx->msg->security = mutt_parse_crypt_hdr (cur->data + 4, 0, APPLICATION_PGP,
                                                    sctx);
        if (sctx->msg->security)
          sctx->msg->security |= APPLICATION_PGP;
        keep = 0;
      }

      if (keep)
      {
        last = &cur->next;
        cur  = cur->next;
      }
      else
      {
        tmp       = cur;
        *last     = cur->next;
        cur       = cur->next;
        tmp->next = NULL;
        mutt_free_list (&tmp);
      }
    }
  }

  rc = 0;

cleanup:
  mutt_buffer_free (&sctx->tempfile);
  return rc;
}

static void label_ref_dec(CONTEXT *ctx, char *label)
{
  struct hash_elem *elem;
  uintptr_t count;

  elem = hash_find_elem (ctx->label_hash, label);
  if (!elem)
    return;

  count = (uintptr_t)elem->data;
  if (count <= 1)
  {
    hash_delete(ctx->label_hash, label, NULL, NULL);
    return;
  }

  count--;
  elem->data = (void *)count;
}

static void label_ref_inc(CONTEXT *ctx, char *label)
{
  struct hash_elem *elem;
  uintptr_t count;

  elem = hash_find_elem (ctx->label_hash, label);
  if (!elem)
  {
    count = 1;
    hash_insert(ctx->label_hash, label, (void *)count);
    return;
  }

  count = (uintptr_t)elem->data;
  count++;
  elem->data = (void *)count;
}

/*
 * add an X-Label: field.
 */
static int label_message(CONTEXT *ctx, HEADER *hdr, char *new)
{
  if (hdr == NULL)
    return 0;
  if (mutt_strcmp (hdr->env->x_label, new) == 0)
    return 0;

  if (hdr->env->x_label != NULL)
    label_ref_dec(ctx, hdr->env->x_label);
  mutt_str_replace (&hdr->env->x_label, new);
  if (hdr->env->x_label != NULL)
    label_ref_inc(ctx, hdr->env->x_label);

  hdr->changed = 1;
  hdr->env->changed |= MUTT_ENV_CHANGED_XLABEL;
  return 1;
}

int mutt_label_message(HEADER *hdr)
{
  char buf[LONG_STRING], *new;
  int i;
  int changed;

  if (!Context || !Context->label_hash)
    return 0;

  *buf = '\0';
  if (hdr != NULL && hdr->env->x_label != NULL)
  {
    strfcpy(buf, hdr->env->x_label, LONG_STRING);
  }

  if (mutt_get_field("Label: ", buf, sizeof(buf), MUTT_LABEL /* | MUTT_CLEAR */) != 0)
    return 0;

  new = buf;
  SKIPWS(new);
  if (*new == '\0')
    new = NULL;

  changed = 0;
  if (hdr != NULL)
  {
    if (label_message(Context, hdr, new))
    {
      ++changed;
      mutt_set_header_color (Context, hdr);
    }
  }
  else
  {
#define HDR_OF(index) Context->hdrs[Context->v2r[(index)]]
    for (i = 0; i < Context->vcount; ++i)
    {
      if (HDR_OF(i)->tagged)
        if (label_message(Context, HDR_OF(i), new))
        {
          ++changed;
          mutt_set_flag(Context, HDR_OF(i),
                        MUTT_TAG, 0);
          /* mutt_set_flag re-evals the header color */
        }
    }
  }

  return changed;
}

void mutt_make_label_hash (CONTEXT *ctx)
{
  /* 131 is just a rough prime estimate of how many distinct
   * labels someone might have in a mailbox.
   */
  ctx->label_hash = hash_create(131, MUTT_HASH_STRDUP_KEYS);
}

void mutt_label_hash_add (CONTEXT *ctx, HEADER *hdr)
{
  if (!ctx || !ctx->label_hash)
    return;
  if (hdr->env->x_label)
    label_ref_inc (ctx, hdr->env->x_label);
}

void mutt_label_hash_remove (CONTEXT *ctx, HEADER *hdr)
{
  if (!ctx || !ctx->label_hash)
    return;
  if (hdr->env->x_label)
    label_ref_dec (ctx, hdr->env->x_label);
}
