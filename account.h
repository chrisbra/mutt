/*
 * Copyright (C) 2000-2007,2012 Brendan Cully <brendan@kublai.com>
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

/* remote host account manipulation (POP/IMAP) */

#ifndef _MUTT_ACCOUNT_H_
#define _MUTT_ACCOUNT_H_ 1

#include "url.h"

/* account types */
enum
{
  MUTT_ACCT_TYPE_NONE = 0,
  MUTT_ACCT_TYPE_IMAP,
  MUTT_ACCT_TYPE_POP,
  MUTT_ACCT_TYPE_SMTP
};

/* account flags */
#define MUTT_ACCT_PORT  (1<<0)
#define MUTT_ACCT_USER  (1<<1)
#define MUTT_ACCT_LOGIN (1<<2)
#define MUTT_ACCT_PASS  (1<<3)
#define MUTT_ACCT_SSL   (1<<4)
/* these are used to regenerate a URL in same form it was parsed */
#define MUTT_ACCT_USER_FROM_URL (1<<5)
#define MUTT_ACCT_PASS_FROM_URL (1<<6)

typedef struct
{
  char user[128];
  char login[128];
  char pass[256];
  char host[128];
  unsigned short port;
  unsigned char type;
  unsigned char flags;
} ACCOUNT;

int mutt_account_match (const ACCOUNT* a1, const ACCOUNT* m2);
int mutt_account_fromurl (ACCOUNT* account, ciss_url_t* url);
void mutt_account_tourl (ACCOUNT* account, ciss_url_t* url, int force_user);
int mutt_account_getuser (ACCOUNT* account);
int mutt_account_getlogin (ACCOUNT* account);
int _mutt_account_getpass (ACCOUNT* account,
                           void (*prompt_func) (char *, size_t, ACCOUNT *));
int mutt_account_getpass (ACCOUNT* account);
void mutt_account_unsetpass (ACCOUNT* account);
int mutt_account_getoauthbearer (ACCOUNT* account, BUFFER *authbearer, int xoauth2);

#endif /* _MUTT_ACCOUNT_H_ */
