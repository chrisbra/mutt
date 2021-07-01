/*
 * Copyright (C) 2019 Kevin J. McCarthy <kevin@8t8.us>
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
#include "mutt_crypt.h"
#include "mime.h"
#include "mutt_idna.h"
#include "mailbox.h"
#include "send.h"
#include "autocrypt.h"
#include "autocrypt_private.h"

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#include <errno.h>

static int autocrypt_dir_init (int can_create)
{
  int rv = 0;
  struct stat sb;
  BUFFER *prompt = NULL;

  if (!stat (AutocryptDir, &sb))
    return 0;

  if (!can_create)
    return -1;

  prompt = mutt_buffer_pool_get ();
  /* L10N:
     %s is a directory.  Mutt is looking for a directory it needs
     for some reason (e.g. autocrypt, header cache, bcache), but it
     doesn't exist.  The prompt is asking whether to create the directory
  */
  mutt_buffer_printf (prompt, _("%s does not exist. Create it?"), AutocryptDir);
  if (mutt_yesorno (mutt_b2s (prompt), MUTT_YES) == MUTT_YES)
  {
    if (mutt_mkdir (AutocryptDir, 0700) < 0)
    {
      /* L10N:
         mkdir() on the directory %s failed.  The second %s is the
         error message returned by libc
      */
      mutt_error ( _("Can't create %s: %s."), AutocryptDir, strerror (errno));
      mutt_sleep (0);
      rv = -1;
    }
  }

  mutt_buffer_pool_release (&prompt);
  return rv;
}

int mutt_autocrypt_init (int can_create)
{
  if (AutocryptDB)
    return 0;

  if (!option (OPTAUTOCRYPT) || !AutocryptDir)
    return -1;

  set_option (OPTIGNOREMACROEVENTS);
  /* The init process can display menus at various points
   * (e.g. browser, pgp key selection).  This allows the screen to be
   * autocleared after each menu, so the subsequent prompts can be
   * read. */
  set_option (OPTMENUPOPCLEARSCREEN);

  if (autocrypt_dir_init (can_create))
    goto bail;

  if (mutt_autocrypt_gpgme_init ())
    goto bail;

  if (mutt_autocrypt_db_init (can_create))
    goto bail;

  unset_option (OPTIGNOREMACROEVENTS);
  unset_option (OPTMENUPOPCLEARSCREEN);

  return 0;

bail:
  unset_option (OPTIGNOREMACROEVENTS);
  unset_option (OPTMENUPOPCLEARSCREEN);
  unset_option (OPTAUTOCRYPT);
  mutt_autocrypt_db_close ();
  return -1;
}

void mutt_autocrypt_cleanup (void)
{
  mutt_autocrypt_db_close ();
}

/* Creates a brand new account.
 * This is used the first time autocrypt is initialized, and
 * in the account menu. */
int mutt_autocrypt_account_init (int prompt)
{
  ADDRESS *addr = NULL;
  BUFFER *keyid = NULL, *keydata = NULL;
  AUTOCRYPT_ACCOUNT *account = NULL;
  int done = 0, rv = -1, prefer_encrypt = 0;

  if (prompt)
  {
    /* L10N:
       The first time mutt is started with $autocrypt set, it will
       create $autocrypt_dir and then prompt to create an autocrypt
       account with this message.
    */
    if (mutt_yesorno (_("Create an initial autocrypt account?"),
                      MUTT_YES) != MUTT_YES)
      return 0;
  }

  keyid = mutt_buffer_pool_get ();
  keydata = mutt_buffer_pool_get ();

  if (From)
  {
    addr = rfc822_cpy_adr_real (From);
    if (!addr->personal && Realname)
    {
      addr->personal = safe_strdup (Realname);
#ifdef EXACT_ADDRESS
      FREE (&addr->val);
#endif
    }
  }

  do
  {
    /* L10N:
       Autocrypt is asking for the email address to use for the
       autocrypt account.  This will generate a key and add a record
       to the database for use in autocrypt operations.
    */
    if (mutt_edit_address (&addr, _("Autocrypt account address: "), 0))
      goto cleanup;
    if (!addr || !addr->mailbox || addr->next)
    {
      /* L10N:
         Autocrypt prompts for an account email address, and requires
         a single address.  This is shown if they entered something invalid,
         nothing, or more than one address for some reason.
      */
      mutt_error (_("Please enter a single email address"));
      mutt_sleep (2);
      done = 0;
    }
    else
      done = 1;
  } while (!done);

  if (mutt_autocrypt_db_account_get (addr, &account) < 0)
    goto cleanup;
  if (account)
  {
    /* L10N:
       When creating an autocrypt account, this message will be displayed
       if there is already an account in the database with the email address
       they just entered.
    */
    mutt_error _("That email address is already assigned to an autocrypt account");
    mutt_sleep (1);
    goto cleanup;
  }

  if (mutt_autocrypt_gpgme_select_or_create_key (addr, keyid, keydata))
    goto cleanup;

  /* L10N:
     Autocrypt has a setting "prefer-encrypt".
     When the recommendation algorithm returns "available" and BOTH
     sender and recipient choose "prefer-encrypt", encryption will be
     automatically enabled.
     Otherwise the UI will show encryption is "available" but the user
     will be required to enable encryption manually.
  */
  if (mutt_yesorno (_("Prefer encryption?"), MUTT_NO) == MUTT_YES)
    prefer_encrypt = 1;

  if (mutt_autocrypt_db_account_insert (addr, mutt_b2s (keyid), mutt_b2s (keydata),
                                        prefer_encrypt))
    goto cleanup;

  rv = 0;

cleanup:
  if (rv)
    /* L10N:
       Error message displayed if creating an autocrypt account failed
       or was aborted by the user.
    */
    mutt_error _("Autocrypt account creation aborted.");
  else
    /* L10N:
       Message displayed after an autocrypt account is successfully created.
    */
    mutt_message _("Autocrypt account creation succeeded");
  mutt_sleep (1);

  mutt_autocrypt_db_account_free (&account);
  rfc822_free_address (&addr);
  mutt_buffer_pool_release (&keyid);
  mutt_buffer_pool_release (&keydata);
  return rv;
}

int mutt_autocrypt_process_autocrypt_header (HEADER *hdr, ENVELOPE *env)
{
  AUTOCRYPTHDR *ac_hdr, *valid_ac_hdr = NULL;
  struct timeval now;
  AUTOCRYPT_PEER *peer = NULL;
  AUTOCRYPT_PEER_HISTORY *peerhist = NULL;
  BUFFER *keyid = NULL;
  int update_db = 0, insert_db = 0, insert_db_history = 0, import_gpg = 0;
  int rv = -1;

  if (!option (OPTAUTOCRYPT))
    return 0;

  if (mutt_autocrypt_init (0))
    return -1;

  if (!hdr || !hdr->content || !env)
    return 0;

  /* 1.1 spec says to skip emails with more than one From header */
  if (!env->from || env->from->next)
    return 0;

  /* 1.1 spec also says to skip multipart/report emails */
  if (hdr->content->type == TYPEMULTIPART &&
      !(ascii_strcasecmp (hdr->content->subtype, "report")))
    return 0;

  /* Ignore emails that appear to be more than a week in the future,
   * since they can block all future updates during that time. */
  gettimeofday (&now, NULL);
  if (hdr->date_sent > (now.tv_sec + 7 * 24 * 60 * 60))
    return 0;

  for (ac_hdr = env->autocrypt; ac_hdr; ac_hdr = ac_hdr->next)
  {
    if (ac_hdr->invalid)
      continue;

    /* NOTE: this assumes the processing is occurring right after
     * mutt_parse_rfc822_line() and the from ADDR is still in the same
     * form (intl) as the autocrypt header addr field */
    if (ascii_strcasecmp (env->from->mailbox, ac_hdr->addr))
      continue;

    /* 1.1 spec says ignore all, if more than one valid header is found. */
    if (valid_ac_hdr)
    {
      valid_ac_hdr = NULL;
      break;
    }
    valid_ac_hdr = ac_hdr;
  }

  if (mutt_autocrypt_db_peer_get (env->from, &peer) < 0)
    goto cleanup;

  if (peer)
  {
    if (hdr->date_sent <= peer->autocrypt_timestamp)
    {
      rv = 0;
      goto cleanup;
    }

    if (hdr->date_sent > peer->last_seen)
    {
      update_db = 1;
      peer->last_seen = hdr->date_sent;
    }

    if (valid_ac_hdr)
    {
      update_db = 1;
      peer->autocrypt_timestamp = hdr->date_sent;
      peer->prefer_encrypt = valid_ac_hdr->prefer_encrypt;
      if (mutt_strcmp (peer->keydata, valid_ac_hdr->keydata))
      {
        import_gpg = 1;
        insert_db_history = 1;
        mutt_str_replace (&peer->keydata, valid_ac_hdr->keydata);
      }
    }
  }
  else if (valid_ac_hdr)
  {
    import_gpg = 1;
    insert_db = 1;
    insert_db_history = 1;
  }

  if (!(import_gpg || insert_db || update_db))
  {
    rv = 0;
    goto cleanup;
  }

  if (!peer)
  {
    peer = mutt_autocrypt_db_peer_new ();
    peer->last_seen = hdr->date_sent;
    peer->autocrypt_timestamp = hdr->date_sent;
    peer->keydata = safe_strdup (valid_ac_hdr->keydata);
    peer->prefer_encrypt = valid_ac_hdr->prefer_encrypt;
  }

  if (import_gpg)
  {
    keyid = mutt_buffer_pool_get ();
    if (mutt_autocrypt_gpgme_import_key (peer->keydata, keyid))
      goto cleanup;
    mutt_str_replace (&peer->keyid, mutt_b2s (keyid));
  }

  if (insert_db &&
      mutt_autocrypt_db_peer_insert (env->from, peer))
    goto cleanup;

  if (update_db &&
      mutt_autocrypt_db_peer_update (peer))
    goto cleanup;

  if (insert_db_history)
  {
    peerhist = mutt_autocrypt_db_peer_history_new ();
    peerhist->email_msgid = safe_strdup (env->message_id);
    peerhist->timestamp = hdr->date_sent;
    peerhist->keydata = safe_strdup (peer->keydata);
    if (mutt_autocrypt_db_peer_history_insert (env->from, peerhist))
      goto cleanup;
  }

  rv = 0;

cleanup:
  mutt_autocrypt_db_peer_free (&peer);
  mutt_autocrypt_db_peer_history_free (&peerhist);
  mutt_buffer_pool_release (&keyid);

  return rv;
}

int mutt_autocrypt_process_gossip_header (HEADER *hdr, ENVELOPE *prot_headers)
{
  ENVELOPE *env;
  AUTOCRYPTHDR *ac_hdr;
  struct timeval now;
  AUTOCRYPT_PEER *peer = NULL;
  AUTOCRYPT_GOSSIP_HISTORY *gossip_hist = NULL;
  ADDRESS *peer_addr, *recips = NULL, *last = NULL, ac_hdr_addr = {0};
  BUFFER *keyid = NULL;
  int update_db = 0, insert_db = 0, insert_db_history = 0, import_gpg = 0;
  int rv = -1;

  if (!option (OPTAUTOCRYPT))
    return 0;

  if (mutt_autocrypt_init (0))
    return -1;

  if (!hdr || !hdr->env || !prot_headers)
    return 0;

  env = hdr->env;

  if (!env->from)
    return 0;

  /* Ignore emails that appear to be more than a week in the future,
   * since they can block all future updates during that time. */
  gettimeofday (&now, NULL);
  if (hdr->date_sent > (now.tv_sec + 7 * 24 * 60 * 60))
    return 0;

  keyid = mutt_buffer_pool_get ();

  /* Normalize the recipient list for comparison */
  last = rfc822_append (&recips, env->to, 0);
  last = rfc822_append (last ? &last : &recips, env->cc, 0);
  rfc822_append (last ? &last : &recips, env->reply_to, 0);
  recips = mutt_remove_adrlist_group_delimiters (recips);
  mutt_autocrypt_db_normalize_addrlist (recips);

  for (ac_hdr = prot_headers->autocrypt_gossip; ac_hdr; ac_hdr = ac_hdr->next)
  {
    if (ac_hdr->invalid)
      continue;

    /* normalize for comparison against recipient list */
    mutt_str_replace (&ac_hdr_addr.mailbox, ac_hdr->addr);
    ac_hdr_addr.is_intl = 1;
    ac_hdr_addr.intl_checked = 1;
    mutt_autocrypt_db_normalize_addrlist (&ac_hdr_addr);

    /* Check to make sure the address is in the recipient list.  Since the
     * addresses are normalized we use strcmp, not ascii_strcasecmp. */
    for (peer_addr = recips; peer_addr; peer_addr = peer_addr->next)
      if (!mutt_strcmp (peer_addr->mailbox, ac_hdr_addr.mailbox))
        break;
    if (!peer_addr)
      continue;

    if (mutt_autocrypt_db_peer_get (peer_addr, &peer) < 0)
      goto cleanup;

    if (peer)
    {
      if (hdr->date_sent <= peer->gossip_timestamp)
      {
        mutt_autocrypt_db_peer_free (&peer);
        continue;
      }

      update_db = 1;
      peer->gossip_timestamp = hdr->date_sent;
      /* This is slightly different from the autocrypt 1.1 spec.
       * Avoid setting an empty peer.gossip_keydata with a value that matches
       * the current peer.keydata. */
      if ((peer->gossip_keydata && mutt_strcmp (peer->gossip_keydata, ac_hdr->keydata)) ||
          (!peer->gossip_keydata && mutt_strcmp (peer->keydata, ac_hdr->keydata)))
      {
        import_gpg = 1;
        insert_db_history = 1;
        mutt_str_replace (&peer->gossip_keydata, ac_hdr->keydata);
      }
    }
    else
    {
      import_gpg = 1;
      insert_db = 1;
      insert_db_history = 1;
    }

    if (!peer)
    {
      peer = mutt_autocrypt_db_peer_new ();
      peer->gossip_timestamp = hdr->date_sent;
      peer->gossip_keydata = safe_strdup (ac_hdr->keydata);
    }

    if (import_gpg)
    {
      if (mutt_autocrypt_gpgme_import_key (peer->gossip_keydata, keyid))
        goto cleanup;
      mutt_str_replace (&peer->gossip_keyid, mutt_b2s (keyid));
    }

    if (insert_db &&
        mutt_autocrypt_db_peer_insert (peer_addr, peer))
      goto cleanup;

    if (update_db &&
        mutt_autocrypt_db_peer_update (peer))
      goto cleanup;

    if (insert_db_history)
    {
      gossip_hist = mutt_autocrypt_db_gossip_history_new ();
      gossip_hist->sender_email_addr = safe_strdup (env->from->mailbox);
      gossip_hist->email_msgid = safe_strdup (env->message_id);
      gossip_hist->timestamp = hdr->date_sent;
      gossip_hist->gossip_keydata = safe_strdup (peer->gossip_keydata);
      if (mutt_autocrypt_db_gossip_history_insert (peer_addr, gossip_hist))
        goto cleanup;
    }

    mutt_autocrypt_db_peer_free (&peer);
    mutt_autocrypt_db_gossip_history_free (&gossip_hist);
    mutt_buffer_clear (keyid);
    update_db = insert_db = insert_db_history = import_gpg = 0;
  }

  rv = 0;

cleanup:
  FREE (&ac_hdr_addr.mailbox);
  rfc822_free_address (&recips);
  mutt_autocrypt_db_peer_free (&peer);
  mutt_autocrypt_db_gossip_history_free (&gossip_hist);
  mutt_buffer_pool_release (&keyid);

  return rv;
}

/* Returns the recommendation.  If the recommendataion is > NO and
 * keylist is not NULL, keylist will be populated with the autocrypt
 * keyids
 */
autocrypt_rec_t mutt_autocrypt_ui_recommendation (HEADER *hdr, char **keylist)
{
  autocrypt_rec_t rv = AUTOCRYPT_REC_OFF;
  AUTOCRYPT_ACCOUNT *account = NULL;
  AUTOCRYPT_PEER *peer = NULL;
  ADDRESS *recip, *recips = NULL, *last = NULL;
  int all_encrypt = 1, has_discourage = 0;
  BUFFER *keylist_buf = NULL;
  const char *matching_key;

  if (!option (OPTAUTOCRYPT) ||
      mutt_autocrypt_init (0) ||
      !hdr ||
      !hdr->env->from ||
      hdr->env->from->next)
  {
    if (keylist)
    {
      /* L10N:
         Error displayed if the user tries to force sending an Autocrypt
         email when the engine is not available.
      */
      mutt_message (_("Autocrypt is not available."));
    }
    return AUTOCRYPT_REC_OFF;
  }

  if (hdr->security & APPLICATION_SMIME)
  {
    if (keylist)
      mutt_message (_("Autocrypt is not available."));
    return AUTOCRYPT_REC_OFF;
  }

  if ((mutt_autocrypt_db_account_get (hdr->env->from, &account) <= 0) ||
      !account->enabled)
  {
    if (keylist)
    {
      /* L10N:
         Error displayed if the user tries to force sending an Autocrypt
         email when the account does not exist or is not enabled.
         %s is the From email address used to look up the Autocrypt account.
      */
      mutt_message (_("Autocrypt is not enabled for %s."),
                    NONULL (hdr->env->from->mailbox));
    }
    goto cleanup;
  }

  keylist_buf = mutt_buffer_pool_get ();
  mutt_buffer_addstr (keylist_buf, account->keyid);

  last = rfc822_append (&recips, hdr->env->to, 0);
  last = rfc822_append (last ? &last : &recips, hdr->env->cc, 0);
  rfc822_append (last ? &last : &recips, hdr->env->bcc, 0);
  recips = mutt_remove_adrlist_group_delimiters (recips);

  rv = AUTOCRYPT_REC_NO;
  if (!recips)
    goto cleanup;

  for (recip = recips; recip; recip = recip->next)
  {
    if (mutt_autocrypt_db_peer_get (recip, &peer) <= 0)
    {
      if (keylist)
        /* L10N:
           %s is an email address.  Autocrypt is scanning for the keyids
           to use to encrypt, but it can't find a valid keyid for this address.
           The message is printed and they are returned to the compose menu.
         */
        mutt_message (_("No (valid) autocrypt key found for %s."), recip->mailbox);
      goto cleanup;
    }

    if (mutt_autocrypt_gpgme_is_valid_key (peer->keyid))
    {
      matching_key = peer->keyid;

      if (!(peer->last_seen && peer->autocrypt_timestamp) ||
          (peer->last_seen - peer->autocrypt_timestamp > 35 * 24 * 60 * 60))
      {
        has_discourage = 1;
        all_encrypt = 0;
      }

      if (!account->prefer_encrypt || !peer->prefer_encrypt)
        all_encrypt = 0;
    }
    else if (mutt_autocrypt_gpgme_is_valid_key (peer->gossip_keyid))
    {
      matching_key = peer->gossip_keyid;

      has_discourage = 1;
      all_encrypt = 0;
    }
    else
    {
      if (keylist)
        mutt_message (_("No (valid) autocrypt key found for %s."), recip->mailbox);
      goto cleanup;
    }

    if (mutt_buffer_len (keylist_buf))
      mutt_buffer_addch (keylist_buf, ' ');
    mutt_buffer_addstr (keylist_buf, matching_key);

    mutt_autocrypt_db_peer_free (&peer);
  }

  if (all_encrypt)
    rv = AUTOCRYPT_REC_YES;
  else if (has_discourage)
    rv = AUTOCRYPT_REC_DISCOURAGE;
  else
    rv = AUTOCRYPT_REC_AVAILABLE;

  if (keylist)
    mutt_str_replace (keylist, mutt_b2s (keylist_buf));

cleanup:
  mutt_autocrypt_db_account_free (&account);
  rfc822_free_address (&recips);
  mutt_autocrypt_db_peer_free (&peer);
  mutt_buffer_pool_release (&keylist_buf);
  return rv;
}

int mutt_autocrypt_set_sign_as_default_key (HEADER *hdr)
{
  int rv = -1;
  AUTOCRYPT_ACCOUNT *account = NULL;

  if (!option (OPTAUTOCRYPT) ||
      mutt_autocrypt_init (0) ||
      !hdr ||
      !hdr->env->from ||
      hdr->env->from->next)
    return -1;

  if (mutt_autocrypt_db_account_get (hdr->env->from, &account) <= 0)
    goto cleanup;
  if (!account->keyid)
    goto cleanup;
  if (!account->enabled)
    goto cleanup;

  mutt_str_replace (&AutocryptSignAs, account->keyid);
  mutt_str_replace (&AutocryptDefaultKey, account->keyid);

  rv = 0;

cleanup:
  mutt_autocrypt_db_account_free (&account);
  return rv;
}


static void write_autocrypt_header_line (FILE *fp, const char *addr,
                                         int prefer_encrypt,
                                         const char *keydata)
{
  int count = 0;

  fprintf (fp, "addr=%s; ", addr);
  if (prefer_encrypt)
    fputs ("prefer-encrypt=mutual; ", fp);
  fputs ("keydata=\n", fp);

  while (*keydata)
  {
    count = 0;
    fputs ("\t", fp);
    while (*keydata && count < 75)
    {
      fputc (*keydata, fp);
      count++;
      keydata++;
    }
    fputs ("\n", fp);
  }
}

int mutt_autocrypt_write_autocrypt_header (ENVELOPE *env, FILE *fp)
{
  int rv = -1;
  AUTOCRYPT_ACCOUNT *account = NULL;

  if (!option (OPTAUTOCRYPT) ||
      mutt_autocrypt_init (0) ||
      !env ||
      !env->from ||
      env->from->next)
    return -1;

  if (mutt_autocrypt_db_account_get (env->from, &account) <= 0)
    goto cleanup;
  if (!account->keydata)
    goto cleanup;
  if (!account->enabled)
    goto cleanup;

  fputs ("Autocrypt: ", fp);
  write_autocrypt_header_line (fp, account->email_addr, account->prefer_encrypt,
                               account->keydata);

  rv = 0;

cleanup:
  mutt_autocrypt_db_account_free (&account);
  return rv;
}

int mutt_autocrypt_write_gossip_headers (ENVELOPE *env, FILE *fp)
{
  AUTOCRYPTHDR *gossip;

  if (!option (OPTAUTOCRYPT) ||
      mutt_autocrypt_init (0) ||
      !env)
    return -1;

  for (gossip = env->autocrypt_gossip; gossip; gossip = gossip->next)
  {
    fputs ("Autocrypt-Gossip: ", fp);
    write_autocrypt_header_line (fp, gossip->addr, 0, gossip->keydata);
  }

  return 0;
}

int mutt_autocrypt_generate_gossip_list (HEADER *hdr)
{
  int rv = -1;
  AUTOCRYPT_PEER *peer = NULL;
  AUTOCRYPT_ACCOUNT *account = NULL;
  ADDRESS *recip, *recips = NULL, *last = NULL;
  AUTOCRYPTHDR *gossip;
  const char *keydata, *addr;
  ENVELOPE *mime_headers;

  if (!option (OPTAUTOCRYPT) ||
      mutt_autocrypt_init (0) ||
      !hdr)
    return -1;

  mime_headers = hdr->content->mime_headers;
  if (!mime_headers)
    mime_headers = hdr->content->mime_headers = mutt_new_envelope ();
  mutt_free_autocrypthdr (&mime_headers->autocrypt_gossip);

  last = rfc822_append (&recips, hdr->env->to, 0);
  last = rfc822_append (last ? &last : &recips, hdr->env->cc, 0);
  recips = mutt_remove_adrlist_group_delimiters (recips);

  for (recip = recips; recip; recip = recip->next)
  {
    /* At this point, we just accept missing keys and include what
     * we can. */
    if (mutt_autocrypt_db_peer_get (recip, &peer) <= 0)
      continue;

    keydata = NULL;
    if (mutt_autocrypt_gpgme_is_valid_key (peer->keyid))
      keydata = peer->keydata;
    else if (mutt_autocrypt_gpgme_is_valid_key (peer->gossip_keyid))
      keydata = peer->gossip_keydata;

    if (keydata)
    {
      gossip = mutt_new_autocrypthdr ();
      gossip->addr = safe_strdup (peer->email_addr);
      gossip->keydata = safe_strdup (keydata);
      gossip->next = mime_headers->autocrypt_gossip;
      mime_headers->autocrypt_gossip = gossip;
    }

    mutt_autocrypt_db_peer_free (&peer);
  }

  for (recip = hdr->env->reply_to; recip; recip = recip->next)
  {
    addr = keydata = NULL;
    if (mutt_autocrypt_db_account_get (recip, &account) > 0)
    {
      addr = account->email_addr;
      keydata = account->keydata;
    }
    else if (mutt_autocrypt_db_peer_get (recip, &peer) > 0)
    {
      addr = peer->email_addr;
      if (mutt_autocrypt_gpgme_is_valid_key (peer->keyid))
        keydata = peer->keydata;
      else if (mutt_autocrypt_gpgme_is_valid_key (peer->gossip_keyid))
        keydata = peer->gossip_keydata;
    }

    if (keydata)
    {
      gossip = mutt_new_autocrypthdr ();
      gossip->addr = safe_strdup (addr);
      gossip->keydata = safe_strdup (keydata);
      gossip->next = mime_headers->autocrypt_gossip;
      mime_headers->autocrypt_gossip = gossip;
    }
    mutt_autocrypt_db_account_free (&account);
    mutt_autocrypt_db_peer_free (&peer);
  }

  rfc822_free_address (&recips);
  mutt_autocrypt_db_account_free (&account);
  mutt_autocrypt_db_peer_free (&peer);
  return rv;
}

/* This is invoked during the first autocrypt initialization,
 * to scan one or more mailboxes for autocrypt headers.
 *
 * Due to the implementation, header-cached headers are not scanned,
 * so this routine just opens up the mailboxes with $header_cache
 * temporarily disabled.
 */
void mutt_autocrypt_scan_mailboxes (void)
{
  int scan;
  BUFFER *folderbuf = NULL;
  CONTEXT *ctx = NULL;

#ifdef USE_HCACHE
  char *old_hdrcache = HeaderCache;
  HeaderCache = NULL;
#endif

  folderbuf = mutt_buffer_pool_get ();

  /* L10N:
     The first time autocrypt is enabled, Mutt will ask to scan
     through one or more mailboxes for Autocrypt: headers.
     Those headers are then captured in the database as peer records
     and used for encryption.
     If this is answered yes, they will be prompted for a mailbox.
  */
  scan = mutt_yesorno (_("Scan a mailbox for autocrypt headers?"),
                       MUTT_YES);
  while (scan == MUTT_YES)
  {
    /* L10N:
       The prompt for a mailbox to scan for Autocrypt: headers
    */
    if ((!mutt_enter_mailbox (_("Scan mailbox"), folderbuf, 0)) &&
        mutt_buffer_len (folderbuf))
    {
      mutt_buffer_expand_path (folderbuf);
      /* NOTE: I am purposely *not* executing folder hooks here,
       * as they can do all sorts of things like push into the getch() buffer.
       * Authentication should be in account-hooks. */
      ctx = mx_open_mailbox (mutt_b2s (folderbuf), MUTT_READONLY, NULL);
      mutt_sleep (1);
      mx_close_mailbox (ctx, NULL);

      FREE (&ctx);
      mutt_buffer_clear (folderbuf);
    }

    /* L10N:
       This is the second prompt to see if the user would like
       to scan more than one mailbox for Autocrypt headers.
       I'm purposely being extra verbose; asking first then prompting
       for a mailbox.  This is because this is a one-time operation
       and I don't want them to accidentally ctrl-g and abort it.
    */
    scan = mutt_yesorno (_("Scan another mailbox for autocrypt headers?"),
                         MUTT_YES);
  }

#ifdef USE_HCACHE
  HeaderCache = old_hdrcache;
#endif
  mutt_buffer_pool_release (&folderbuf);
}
