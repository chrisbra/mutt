/*
 * Copyright (C) 1996-1998 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2000-2007,2017 Brendan Cully <brendan@kublai.com>
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

#ifndef _IMAP_H
#define _IMAP_H 1

#include "account.h"
#include "browser.h"
#include "mailbox.h"

/* -- data structures -- */
typedef struct
{
  ACCOUNT account;
  char* mbox;
} IMAP_MBOX;

/* imap.c */
int imap_access (const char *path);
int imap_check_mailbox (CONTEXT *ctx, int *index_hint, int force);
int imap_delete_mailbox (CONTEXT* idata, IMAP_MBOX mx);
int imap_sync_mailbox (CONTEXT *ctx, int expunge, int *index_hint);
int imap_close_mailbox (CONTEXT *ctx);
int imap_buffy_check (int force, int check_stats);
int imap_status (const char *path, int queue);
int imap_search (CONTEXT* ctx, const pattern_t* pat);
int imap_subscribe (char *path, int subscribe);
int imap_complete (char* dest, size_t dlen, const char* path);
int imap_fast_trash (CONTEXT* ctx, char* dest);

void imap_allow_reopen (CONTEXT *ctx);
void imap_disallow_reopen (CONTEXT *ctx);

extern struct mx_ops mx_imap_ops;

/* browse.c */
int imap_browse (const char* path, struct browser_state* state);
int imap_mailbox_create (const char* folder, BUFFER *result);
int imap_mailbox_rename (const char* mailbox, BUFFER *result);

/* message.c */
int imap_append_message (CONTEXT* ctx, MESSAGE* msg);
int imap_copy_messages (CONTEXT* ctx, HEADER* h, const char* dest, int delete);

/* socket.c */
void imap_logout_all (void);

/* util.c */
int imap_expand_path (BUFFER* path);
int imap_buffer_remove_path_password (BUFFER *dest, const char *src);
int imap_parse_path (const char* path, IMAP_MBOX* mx);
void imap_pretty_mailbox (char* path, size_t pathlen);

int imap_wait_keepalive (pid_t pid);
void imap_keepalive (void);

int imap_account_match (const ACCOUNT* a1, const ACCOUNT* a2);

#endif
