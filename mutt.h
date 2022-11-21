/*
 * Copyright (C) 1996-2002,2010,2013 Michael R. Elkins <me@mutt.org>
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

#ifndef MUTT_H
#define MUTT_H

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h> /* needed for SEEK_SET */
#ifdef HAVE_UNIX_H
# include <unix.h>   /* needed for snprintf on QNX. */
#endif
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>
#include <limits.h>
#include <stdarg.h>
#include <signal.h>
/* On OS X 10.5.x, wide char functions are inlined by default breaking
 * --without-wc-funcs compilation
 */
#ifdef __APPLE_CC__
#define _DONT_USE_CTYPE_INLINE_
#endif
#ifdef HAVE_WCHAR_H
# include <wchar.h>
#endif
#if defined(HAVE_WCTYPE_H) && defined(HAVE_WC_FUNCS)
# include <wctype.h>
#endif

#ifndef _POSIX_PATH_MAX
#include <limits.h>
#endif

/* PATH_MAX is undefined on the hurd */
#if !defined(PATH_MAX) && defined(_POSIX_PATH_MAX)
#define PATH_MAX _POSIX_PATH_MAX
#endif

#include <pwd.h>
#include <grp.h>

#include "rfc822.h"
#include "hash.h"
#include "charset.h"
#include "buffer.h"
#include "color.h"

#ifndef HAVE_WC_FUNCS
# ifdef MB_LEN_MAX
#  undef MB_LEN_MAX
# endif
# define MB_LEN_MAX 16
#endif

#ifdef HAVE_FGETS_UNLOCKED
# ifdef fgets
#  undef fgets
# endif
# define fgets fgets_unlocked
#endif

#ifdef HAVE_FGETC_UNLOCKED
# ifdef fgetc
#  undef fgetc
# endif
# define fgetc fgetc_unlocked
#endif

#ifndef HAVE_STRUCT_TIMESPEC
struct timespec
{
  time_t tv_sec;
  long tv_nsec;
};
#endif

/* nifty trick I stole from ELM 2.5alpha. */
#ifdef MAIN_C
#define WHERE
#define INITVAL(x) = x
#else
#define WHERE extern
#define INITVAL(x)
#endif

#define WHERE_DEFINED 1

#include "mutt_regex.h"

/* flags for mutt_enter_string() */
#define  MUTT_ALIAS    1      /* do alias "completion" by calling up the alias-menu */
#define  MUTT_FILE     (1<<1) /* do file completion, file history ring */
#define  MUTT_MAILBOX  (1<<2) /* do file completion, mailbox history ring */
#define  MUTT_INCOMING (1<<3) /* do incoming folders buffy cycle */
#define  MUTT_CMD      (1<<4) /* do completion on previous word */
#define  MUTT_PASS     (1<<5) /* password mode (no echo) */
#define  MUTT_CLEAR    (1<<6) /* clear input if printable character is pressed */
#define  MUTT_COMMAND  (1<<7) /* do command completion */
#define  MUTT_PATTERN  (1<<8) /* pattern mode - only used for history classes */
#define  MUTT_LABEL    (1<<9) /* do label completion */

/* flags for mutt_get_token() */
#define MUTT_TOKEN_EQUAL      1       /* treat '=' as a special */
#define MUTT_TOKEN_CONDENSE   (1<<1)  /* ^(char) to control chars (macros) */
#define MUTT_TOKEN_SPACE      (1<<2)  /* don't treat whitespace as a term */
#define MUTT_TOKEN_QUOTE      (1<<3)  /* don't interpret quotes */
#define MUTT_TOKEN_PATTERN    (1<<4)  /* !)|~ are terms (for patterns) */
#define MUTT_TOKEN_COMMENT    (1<<5)  /* don't reap comments */
#define MUTT_TOKEN_SEMICOLON  (1<<6)  /* don't treat ; as special */
#define MUTT_TOKEN_ESC_VARS   (1<<7)  /* escape configuration variables */
#define MUTT_TOKEN_LISP       (1<<8)  /* enable lisp processing */
#define MUTT_TOKEN_NOLISP     (1<<9)  /* force-disable lisp, ignoring $lisp_args */

typedef struct
{
  int ch; /* raw key pressed */
  int op; /* function op */
} event_t;

/* flags for _mutt_system() */
#define MUTT_DETACH_PROCESS	1	/* detach subprocess from group */

/* flags for mutt_get_stat_timespec */
typedef enum
{
  MUTT_STAT_ATIME,
  MUTT_STAT_MTIME,
  MUTT_STAT_CTIME
} mutt_stat_type;

/* flags for mutt_FormatString() */
typedef enum
{
  MUTT_FORMAT_FORCESUBJ   = (1<<0), /* print the subject even if unchanged */
  MUTT_FORMAT_TREE        = (1<<1), /* draw the thread tree */
  MUTT_FORMAT_OPTIONAL    = (1<<2),
  MUTT_FORMAT_STAT_FILE   = (1<<3), /* used by mutt_attach_fmt */
  MUTT_FORMAT_ARROWCURSOR = (1<<4), /* reserve space for arrow_cursor */
  MUTT_FORMAT_INDEX       = (1<<5), /* this is a main index entry */
  MUTT_FORMAT_NOFILTER    = (1<<6)  /* do not allow filtering on this pass */
} format_flag;

/* mode for mutt_write_rfc822_header() */
typedef enum
{
  MUTT_WRITE_HEADER_NORMAL,
  MUTT_WRITE_HEADER_FCC,
  MUTT_WRITE_HEADER_POSTPONE,
  MUTT_WRITE_HEADER_EDITHDRS,
  MUTT_WRITE_HEADER_MIME
} mutt_write_header_mode;

/* types for mutt_add_hook() */
#define MUTT_FOLDERHOOK  1
#define MUTT_MBOXHOOK    (1<<1)
#define MUTT_SENDHOOK    (1<<2)
#define MUTT_FCCHOOK     (1<<3)
#define MUTT_SAVEHOOK    (1<<4)
#define MUTT_CHARSETHOOK (1<<5)
#define MUTT_ICONVHOOK   (1<<6)
#define MUTT_MESSAGEHOOK (1<<7)
#define MUTT_CRYPTHOOK   (1<<8)
#define MUTT_ACCOUNTHOOK (1<<9)
#define MUTT_REPLYHOOK   (1<<10)
#define MUTT_SEND2HOOK   (1<<11)
#ifdef USE_COMPRESSED
#define MUTT_OPENHOOK    (1<<12)
#define MUTT_APPENDHOOK  (1<<13)
#define MUTT_CLOSEHOOK   (1<<14)
#endif /* USE_COMPRESSED */
#define MUTT_IDXFMTHOOK  (1<<15)

/* tree characters for linearize_tree and print_enriched_string */
#define MUTT_TREE_LLCORNER      1
#define MUTT_TREE_ULCORNER      2
#define MUTT_TREE_LTEE          3
#define MUTT_TREE_HLINE         4
#define MUTT_TREE_VLINE         5
#define MUTT_TREE_SPACE         6
#define MUTT_TREE_RARROW        7
#define MUTT_TREE_STAR          8
#define MUTT_TREE_HIDDEN        9
#define MUTT_TREE_EQUALS        10
#define MUTT_TREE_TTEE          11
#define MUTT_TREE_BTEE          12
#define MUTT_TREE_MISSING       13
#define MUTT_TREE_MAX           14

#define MUTT_THREAD_COLLAPSE    (1<<0)
#define MUTT_THREAD_UNCOLLAPSE  (1<<1)
#define MUTT_THREAD_UNREAD      (1<<2)
#define MUTT_THREAD_NEXT_UNREAD (1<<3)

enum
{
  /* modes for mutt_view_attachment() */
  MUTT_REGULAR = 1,
  MUTT_MAILCAP,
  MUTT_VIEW_PAGER,
  MUTT_AS_TEXT,

  /* action codes used by mutt_set_flag() and mutt_pattern_function() */
  MUTT_ALL,
  MUTT_NONE,
  MUTT_NEW,
  MUTT_OLD,
  MUTT_REPLIED,
  MUTT_READ,
  MUTT_UNREAD,
  MUTT_DELETE,
  MUTT_UNDELETE,
  MUTT_PURGE,
  MUTT_DELETED,
  MUTT_FLAG,
  MUTT_TAG,
  MUTT_UNTAG,
  MUTT_LIMIT,
  MUTT_EXPIRED,
  MUTT_SUPERSEDED,
  MUTT_TRASH,

  /* actions for mutt_pattern_comp/mutt_pattern_exec */
  MUTT_AND,
  MUTT_OR,
  MUTT_THREAD,
  MUTT_PARENT,
  MUTT_CHILDREN,
  MUTT_TO,
  MUTT_CC,
  MUTT_COLLAPSED,
  MUTT_SUBJECT,
  MUTT_FROM,
  MUTT_DATE,
  MUTT_DATE_RECEIVED,
  MUTT_DUPLICATED,
  MUTT_UNREFERENCED,
  MUTT_ID,
  MUTT_BODY,
  MUTT_HEADER,
  MUTT_HORMEL,
  MUTT_WHOLE_MSG,
  MUTT_SENDER,
  MUTT_MESSAGE,
  MUTT_SCORE,
  MUTT_SIZE,
  MUTT_REFERENCE,
  MUTT_RECIPIENT,
  MUTT_LIST,
  MUTT_SUBSCRIBED_LIST,
  MUTT_PERSONAL_RECIP,
  MUTT_PERSONAL_FROM,
  MUTT_ADDRESS,
  MUTT_CRYPT_SIGN,
  MUTT_CRYPT_VERIFIED,
  MUTT_CRYPT_ENCRYPT,
  MUTT_PGP_KEY,
  MUTT_XLABEL,
  MUTT_MIMEATTACH,
  MUTT_MIMETYPE,

  /* Options for Mailcap lookup */
  MUTT_EDIT,
  MUTT_COMPOSE,
  MUTT_PRINT,
  MUTT_AUTOVIEW,

  /* options for socket code */
  MUTT_NEW_SOCKET,
#ifdef USE_SSL_OPENSSL
  MUTT_NEW_SSL_SOCKET,
#endif

  /* Options for mutt_save_attachment */
  MUTT_SAVE_APPEND,
  MUTT_SAVE_OVERWRITE
};

/* used by init.h MuttVars and Commands dispatch functions */
/* possible arguments to set_quadoption() */
union pointer_long_t
{
  void *p;
  long l;
};

enum
{
  MUTT_NO,
  MUTT_YES,
  MUTT_ASKNO,
  MUTT_ASKYES
};

/* quad-option vars */
enum
{
  OPT_ABORT,
  OPT_ABORTNOATTACH,
  OPT_ATTACH_SAVE_CHARCONV,
  OPT_BOUNCE,
  OPT_COPY,
  OPT_DELETE,
  OPT_FORWATTS,
  OPT_FORWEDIT,
  OPT_FORWDECRYPT,
  OPT_FCCATTACH,
  OPT_INCLUDE,
  OPT_MFUPTO,
  OPT_MIMEFWD,
  OPT_MIMEFWDREST,
  OPT_MOVE,
  OPT_PGPMIMEAUTO,     /* ask to revert to PGP/MIME when inline fails */
#ifdef USE_POP
  OPT_POPDELETE,
  OPT_POPRECONNECT,
#endif
  OPT_POSTPONE,
  OPT_PRINT,
  OPT_QUIT,
  OPT_REPLYTO,
  OPT_RECALL,
  OPT_SENDMULTIPARTALT,
#if defined(USE_SSL)
  OPT_SSLSTARTTLS,
#endif
  OPT_SUBJECT,
  OPT_VERIFYSIG,      /* verify PGP signatures */

  /* THIS MUST BE THE LAST VALUE. */
  OPT_MAX
};

/* flags to mutt_send_message() */
#define SENDREPLY	(1<<0)
#define SENDGROUPREPLY	(1<<1)
#define SENDLISTREPLY	(1<<2)
#define SENDFORWARD	(1<<3)
#define SENDPOSTPONED	(1<<4)
#define SENDBATCH	(1<<5)
#define SENDMAILX	(1<<6)
#define SENDKEY		(1<<7)
#define SENDRESEND	(1<<8)
#define SENDPOSTPONEDFCC	(1<<9) /* used by mutt_get_postponed() to signal that the x-mutt-fcc header field was present */
#define SENDNOFREEHEADER	(1<<10)   /* Used by the -E flag */
#define SENDDRAFTFILE		(1<<11)   /* Used by the -H flag */
#define SENDTOSENDER            (1<<12)
#define SENDGROUPCHATREPLY      (1<<13)
#define SENDBACKGROUNDEDIT      (1<<14)  /* Allow background editing */
#define SENDCHECKPOSTPONED      (1<<15)  /* Check for postponed messages */

/* flags for mutt_edit_headers() */
#define MUTT_EDIT_HEADERS_BACKGROUND  1
#define MUTT_EDIT_HEADERS_RESUME      2

/* flags to _mutt_select_file() */
#define MUTT_SEL_BUFFY  (1<<0)
#define MUTT_SEL_MULTI  (1<<1)
#define MUTT_SEL_FOLDER (1<<2)
#define MUTT_SEL_DIRECTORY (1<<3)  /* Allow directories to be selected
                                    * via <view-file> */

/* flags for parse_spam_list */
#define MUTT_SPAM          1
#define MUTT_NOSPAM        2

/* flags for _mutt_set_flag() */
#define MUTT_SET_FLAG_UPDATE_CONTEXT  (1<<0)

/* flags for _mutt_buffer_expand_path() */
#define MUTT_EXPAND_PATH_RX                     (1<<0)
#define MUTT_EXPAND_PATH_EXPAND_RELATIVE        (1<<1)
#define MUTT_EXPAND_PATH_REMOVE_TRAILING_SLASH  (1<<2)

/* boolean vars */
enum
{
  OPTALLOW8BIT,
  OPTALLOWANSI,
  OPTARROWCURSOR,
  OPTASCIICHARS,
  OPTASKBCC,
  OPTASKCC,
  OPTATTACHSPLIT,
#ifdef USE_AUTOCRYPT
  OPTAUTOCRYPT,
  OPTAUTOCRYPTREPLY,
#endif
  OPTAUTOEDIT,
  OPTAUTOSUBSCRIBE,
  OPTAUTOTAG,
  OPTBEEP,
  OPTBACKGROUNDEDIT,
  OPTBACKGROUNDCONFIRMQUIT,
  OPTBEEPNEW,
  OPTBOUNCEDELIVERED,
  OPTCHANGEFOLDERNEXT,
  OPTBRAILLEFRIENDLY,
  OPTBROWSERABBRMAILBOXES,
  OPTBROWSERSTICKYCURSOR,
  OPTCHECKMBOXSIZE,
  OPTCHECKNEW,
  OPTCOLLAPSEUNREAD,
  OPTCOMPOSECONFIRMDETACH,
  OPTCONFIRMAPPEND,
  OPTCONFIRMCREATE,
  OPTCOPYDECODEWEED,
  OPTCOUNTALTERNATIVES,
  OPTCURSOROVERLAY,
  OPTDELETEUNTAG,
  OPTDIGESTCOLLAPSE,
  OPTDUPTHREADS,
  OPTEDITHDRS,
  OPTENCODEFROM,
  OPTENVFROM,
  OPTFASTREPLY,
  OPTFCCBEFORESEND,
  OPTFCCCLEAR,
  OPTFLAGSAFE,
  OPTFOLLOWUPTO,
  OPTFORCENAME,
  OPTFORWDECODE,
  OPTFORWQUOTE,
#ifdef USE_HCACHE
  OPTHCACHEVERIFY,
#if defined(HAVE_QDBM) || defined(HAVE_TC) || defined(HAVE_KC)
  OPTHCACHECOMPRESS,
#endif /* HAVE_QDBM */
#endif
  OPTHDRS,
  OPTHEADER,
  OPTHEADERCOLORPARTIAL,
  OPTHELP,
  OPTHIDDENHOST,
  OPTHIDELIMITED,
  OPTHIDEMISSING,
  OPTHIDETHREADSUBJECT,
  OPTHIDETOPLIMITED,
  OPTHIDETOPMISSING,
  OPTHISTREMOVEDUPS,
  OPTHONORDISP,
  OPTIGNORELWS,
  OPTIGNORELISTREPLYTO,
#ifdef USE_IMAP
  OPTIMAPCHECKSUBSCRIBED,
  OPTIMAPCONDSTORE,
  OPTIMAPIDLE,
  OPTIMAPLSUB,
  OPTIMAPPASSIVE,
  OPTIMAPPEEK,
  OPTIMAPQRESYNC,
  OPTIMAPSERVERNOISE,
#ifdef USE_ZLIB
  OPTIMAPDEFLATE,
#endif
#endif
#if defined(USE_SSL)
# ifndef USE_SSL_GNUTLS
  OPTSSLSYSTEMCERTS,
  OPTSSLV2,
# endif /* USE_SSL_GNUTLS */
  OPTSSLV3,
  OPTTLSV1,
  OPTTLSV1_1,
  OPTTLSV1_2,
  OPTTLSV1_3,
  OPTSSLFORCETLS,
  OPTSSLVERIFYDATES,
  OPTSSLVERIFYHOST,
# if defined(USE_SSL_OPENSSL) && defined(HAVE_SSL_PARTIAL_CHAIN)
  OPTSSLVERIFYPARTIAL,
# endif /* USE_SSL_OPENSSL */
#endif /* defined(USE_SSL) */
  OPTIMPLICITAUTOVIEW,
  OPTINCLUDEENCRYPTED,
  OPTINCLUDEONLYFIRST,
  OPTKEEPFLAGGED,
  OPTLOCALDATEHEADER,
  OPTMUTTLISPINLINEEVAL,
  OPTMAILCAPSANITIZE,
  OPTMAILCHECKRECENT,
  OPTMAILCHECKSTATS,
  OPTMAILDIRTRASH,
  OPTMAILDIRCHECKCUR,
  OPTMARKERS,
  OPTMARKOLD,
  OPTMENUSCROLL,	/* scroll menu instead of implicit next-page */
  OPTMENUMOVEOFF,	/* allow menu to scroll past last entry */
#if defined(USE_IMAP) || defined(USE_POP)
  OPTMESSAGECACHECLEAN,
#endif
  OPTMETAKEY,		/* interpret ALT-x as ESC-x */
  OPTMETOO,
  OPTMHPURGE,
  OPTMIMEFORWDECODE,
  OPTMIMETYPEQUERYFIRST,
  OPTNARROWTREE,
  OPTPAGERSTOP,
  OPTPIPEDECODE,
  OPTPIPEDECODEWEED,
  OPTPIPESPLIT,
#ifdef USE_POP
  OPTPOPAUTHTRYALL,
  OPTPOPLAST,
#endif
  OPTPOSTPONEENCRYPT,
  OPTPRINTDECODE,
  OPTPRINTDECODEWEED,
  OPTPRINTSPLIT,
  OPTPROMPTAFTER,
  OPTREADONLY,
  OPTREFLOWSPACEQUOTES,
  OPTREFLOWTEXT,
  OPTREPLYSELF,
  OPTRESOLVE,
  OPTRESUMEDRAFTFILES,
  OPTRESUMEEDITEDDRAFTFILES,
  OPTREVALIAS,
  OPTREVNAME,
  OPTREVREAL,
  OPTRFC2047PARAMS,
  OPTSAVEADDRESS,
  OPTSAVEEMPTY,
  OPTSAVENAME,
  OPTSCORE,
#ifdef USE_SIDEBAR
  OPTSIDEBAR,
  OPTSIDEBARFOLDERINDENT,
  OPTSIDEBARNEWMAILONLY,
  OPTSIDEBARNEXTNEWWRAP,
  OPTSIDEBARRELSPINDENT,
  OPTSIDEBARUSEMBSHORTCUTS,
  OPTSIDEBARSHORTPATH,
#endif
  OPTSIGDASHES,
  OPTSIGONTOP,
  OPTSIZESHOWBYTES,
  OPTSIZESHOWMB,
  OPTSIZESHOWFRACTIONS,
  OPTSIZEUNITSONLEFT,
  OPTSORTRE,
  OPTSPAMSEP,
  OPTSTATUSONTOP,
  OPTSTRICTTHREADS,
  OPTSUSPEND,
  OPTTEXTFLOWED,
  OPTTHOROUGHSRC,
  OPTTHREADRECEIVED,
  OPTTILDE,
  OPTTSENABLED,
  OPTTUNNELISSECURE,
  OPTUNCOLLAPSEJUMP,
  OPTUNCOLLAPSENEW,
  OPTUSE8BITMIME,
  OPTUSEDOMAIN,
  OPTUSEFROM,
  OPTUSEGPGAGENT,
#if defined(HAVE_LIBIDN) || defined(HAVE_LIBIDN2)
  OPTIDNDECODE,
  OPTIDNENCODE,
#endif
#ifdef HAVE_GETADDRINFO
  OPTUSEIPV6,
#endif
  OPTWAITKEY,
  OPTWEED,
  OPTWRAP,
  OPTWRAPSEARCH,
  OPTWRITEBCC,		/* write out a bcc header? */
  OPTXMAILER,

  OPTCRYPTUSEGPGME,
  OPTCRYPTUSEPKA,

  /* PGP options */

  OPTCRYPTAUTOSIGN,
  OPTCRYPTAUTOENCRYPT,
  OPTCRYPTAUTOPGP,
  OPTCRYPTAUTOSMIME,
  OPTCRYPTCONFIRMHOOK,
  OPTCRYPTOPPORTUNISTICENCRYPT,
  OPTCRYPTOPPENCSTRONGKEYS,
  OPTCRYPTPROTHDRSREAD,
  OPTCRYPTPROTHDRSSAVE,
  OPTCRYPTPROTHDRSWRITE,
  OPTCRYPTREPLYENCRYPT,
  OPTCRYPTREPLYSIGN,
  OPTCRYPTREPLYSIGNENCRYPTED,
  OPTCRYPTTIMESTAMP,
  OPTSMIMEISDEFAULT,
  OPTSMIMESELFENCRYPT,
  OPTASKCERTLABEL,
  OPTSDEFAULTDECRYPTKEY,
  OPTPGPIGNORESUB,
  OPTPGPCHECKEXIT,
  OPTPGPCHECKGPGDECRYPTSTATUSFD,
  OPTPGPLONGIDS,
  OPTPGPAUTODEC,
  OPTPGPRETAINABLESIG,
  OPTPGPSELFENCRYPT,
  OPTPGPSTRICTENC,
  OPTPGPSHOWUNUSABLE,
  OPTPGPAUTOINLINE,
  OPTPGPREPLYINLINE,

  /* pseudo options */

  OPTAUXSORT,		/* (pseudo) using auxiliary sort function */
  OPTFORCEREFRESH,	/* (pseudo) refresh even during macros */
  OPTLOCALES,		/* (pseudo) set if user has valid locale definition */
  OPTNOCURSES,		/* (pseudo) when sending in batch mode */
  OPTSEARCHREVERSE,	/* (pseudo) used by ci_search_command */
  OPTMSGERR,		/* (pseudo) used by mutt_error/mutt_message */
  OPTSEARCHINVALID,	/* (pseudo) used to invalidate the search pat */
  OPTSIGNALSBLOCKED,	/* (pseudo) using by mutt_block_signals () */
  OPTSYSSIGNALSBLOCKED,	/* (pseudo) using by mutt_block_signals_system () */
  OPTNEEDRESORT,	/* (pseudo) used to force a re-sort */
  OPTRESORTINIT,	/* (pseudo) used to force the next resort to be from scratch */
  OPTVIEWATTACH,	/* (pseudo) signals that we are viewing attachments */
  OPTSORTSUBTHREADS,	/* (pseudo) used when $sort_aux changes */
  OPTNEEDRESCORE,	/* (pseudo) set when the `score' command is used */
  OPTATTACHMSG,		/* (pseudo) used by attach-message */
  OPTKEEPQUIET,		/* (pseudo) shut up the message and refresh
			 * 	    functions while we are executing an
			 * 	    external program.
			 */
  OPTMENUCALLER,	/* (pseudo) tell menu to give caller a take */
  OPTREDRAWTREE,	/* (pseudo) redraw the thread tree */
  OPTPGPCHECKTRUST,	/* (pseudo) used by pgp_select_key () */
  OPTDONTHANDLEPGPKEYS,	/* (pseudo) used to extract PGP keys */
  OPTIGNOREMACROEVENTS, /* (pseudo) don't process macro/push/exec events while set */
  OPTAUTOCRYPTGPGME,    /* (pseudo) use Autocrypt context inside crypt-gpgme.c */
  OPTMENUPOPCLEARSCREEN, /* (pseudo) clear the screen when popping the last menu. */

  OPTMAX
};

#define mutt_bit_alloc(n) calloc ((n + 7) / 8, sizeof (char))
#define mutt_bit_set(v,n) v[n/8] |= (1 << (n % 8))
#define mutt_bit_unset(v,n) v[n/8] &= ~(1 << (n % 8))
#define mutt_bit_toggle(v,n) v[n/8] ^= (1 << (n % 8))
#define mutt_bit_isset(v,n) (v[n/8] & (1 << (n % 8)))

#define set_option(x) mutt_bit_set(Options,x)
#define unset_option(x) mutt_bit_unset(Options,x)
#define toggle_option(x) mutt_bit_toggle(Options,x)
#define option(x) mutt_bit_isset(Options,x)

typedef struct list_t
{
  char *data;
  struct list_t *next;
} LIST;

typedef struct rx_list_t
{
  REGEXP *rx;
  struct rx_list_t *next;
} RX_LIST;

typedef struct replace_list_t
{
  REGEXP *rx;
  int     nmatch;
  char   *template;
  struct replace_list_t *next;
} REPLACE_LIST;

#define mutt_new_list() safe_calloc (1, sizeof (LIST))
#define mutt_new_rx_list() safe_calloc (1, sizeof (RX_LIST))
#define mutt_new_replace_list() safe_calloc (1, sizeof (REPLACE_LIST))
void mutt_free_list (LIST **);
void mutt_free_list_generic (LIST **list, void (*data_free)(char **));
void mutt_free_rx_list (RX_LIST **);
void mutt_free_replace_list (REPLACE_LIST **);
LIST *mutt_copy_list (LIST *);
int mutt_matches_ignore (const char *, LIST *);

/* add an element to a list */
LIST *mutt_add_list (LIST *, const char *);
LIST *mutt_add_list_n (LIST*, const void *, size_t);
LIST *mutt_find_list (LIST *, const char *);
int mutt_remove_from_rx_list (RX_LIST **l, const char *str);

void mutt_init (int, LIST *);

typedef struct alias
{
  struct alias *self;		/* XXX - ugly hack */
  char *name;
  ADDRESS *addr;
  struct alias *next;
  short tagged;
  short del;
  short num;
} ALIAS;

/* Flags for envelope->changed.
 * Note that additions to this list also need to be added to:
 *   mutt_copy_header()
 *   mutt_merge_envelopes()
 *   imap_reconnect()
 */
#define MUTT_ENV_CHANGED_IRT     (1<<0)  /* In-Reply-To changed to link/break threads */
#define MUTT_ENV_CHANGED_REFS    (1<<1)  /* References changed to break thread */
#define MUTT_ENV_CHANGED_XLABEL  (1<<2)  /* X-Label edited */
#define MUTT_ENV_CHANGED_SUBJECT (1<<3)  /* Protected header update */

#ifdef USE_AUTOCRYPT
typedef struct autocrypt
{
  char *addr;
  char *keydata;
  unsigned int prefer_encrypt : 1;
  unsigned int invalid : 1;
  struct autocrypt *next;
} AUTOCRYPTHDR;
#endif

typedef struct envelope
{
  ADDRESS *return_path;
  ADDRESS *from;
  ADDRESS *to;
  ADDRESS *cc;
  ADDRESS *bcc;
  ADDRESS *sender;
  ADDRESS *reply_to;
  ADDRESS *mail_followup_to;
  char *list_post;
  char *subject;
  char *real_subj;      /* offset of the real subject */
  char *disp_subj;      /* display subject (modified copy of subject) */
  char *message_id;
  char *supersedes;
  char *date;
  char *x_label;
  BUFFER *spam;
  LIST *references;		/* message references (in reverse order) */
  LIST *in_reply_to;		/* in-reply-to header content */
  LIST *userhdrs;		/* user defined headers */
#ifdef USE_AUTOCRYPT
  AUTOCRYPTHDR *autocrypt;
  AUTOCRYPTHDR *autocrypt_gossip;
#endif
  unsigned char changed;       /* The MUTT_ENV_CHANGED_* flags specify which
                                * fields are modified */
} ENVELOPE;

typedef struct parameter
{
  char *attribute;
  char *value;
  struct parameter *next;
} PARAMETER;

/* Information that helps in determining the Content-* of an attachment */
typedef struct content
{
  long hibin;              /* 8-bit characters */
  long lobin;              /* unprintable 7-bit chars (eg., control chars) */
  long nulbin;             /* null characters (0x0) */
  long crlf;		   /* '\r' and '\n' characters */
  long ascii;              /* number of ascii chars */
  long linemax;            /* length of the longest line in the file */
  unsigned int space : 1;  /* whitespace at the end of lines? */
  unsigned int binary : 1; /* long lines, or CR not in CRLF pair */
  unsigned int from : 1;   /* has a line beginning with "From "? */
  unsigned int dot : 1;    /* has a line consisting of a single dot? */
  unsigned int cr : 1;     /* has CR, even when in a CRLF pair */
} CONTENT;

typedef struct body
{
  char *xtype;			/* content-type if x-unknown */
  char *subtype;                /* content-type subtype */
  PARAMETER *parameter;         /* parameters of the content-type */
  char *description;            /* content-description */
  char *form_name;		/* Content-Disposition form-data name param */
  LOFF_T hdr_offset;            /* offset in stream where the headers begin.
				 * this info is used when invoking metamail,
				 * where we need to send the headers of the
				 * attachment
				 */
  LOFF_T offset;                /* offset where the actual data begins */
  LOFF_T length;                /* length (in bytes) of attachment */
  char *filename;               /* when sending a message, this is the file
				 * to which this structure refers
				 */
  char *d_filename;		/* filename to be used for the
				 * content-disposition header.
				 * If NULL, filename is used
				 * instead.
				 */
  char *charset;                /* send mode: charset of attached file as stored
                                 * on disk.  the charset used in the generated
                                 * message is stored in parameter. */
  CONTENT *content;             /* structure used to store detailed info about
				 * the content of the attachment.  this is used
				 * to determine what content-transfer-encoding
				 * is required when sending mail.
				 */
  struct body *next;            /* next attachment in the list */
  struct body *parts;           /* parts of a multipart or message/rfc822 */
  struct header *hdr;		/* header information for message/rfc822 */

  struct attachptr *aptr;	/* Menu information, used in recvattach.c */

  signed short attach_count;

  time_t stamp;			/* time stamp of last
				 * encoding update.
				 */

  struct envelope *mime_headers; /* memory hole protected headers */

  unsigned int type : 4;        /* content-type primary type */
  unsigned int encoding : 3;    /* content-transfer-encoding */
  unsigned int disposition : 2; /* content-disposition */
  unsigned int use_disp : 1;    /* Content-Disposition uses filename= ? */
  unsigned int unlink : 1;      /* flag to indicate the the file named by
				 * "filename" should be unlink()ed before
				 * free()ing this structure
				 */
  unsigned int tagged : 1;
  unsigned int deleted : 1;	/* attachment marked for deletion */

  unsigned int noconv : 1;	/* don't do character set conversion */
  unsigned int force_charset : 1;  /* send mode: don't adjust the character
                                    * set when in send-mode.
                                    */
  unsigned int is_signed_data : 1; /* A lot of MUAs don't indicate
                                      S/MIME signed-data correctly,
                                      e.g. they use foo.p7m even for
                                      the name of signed data.  This
                                      flag is used to keep track of
                                      the actual message type.  It
                                      gets set during the verification
                                      (which is done if the encryption
                                      try failed) and check by the
                                      function to figure the type of
                                      the message. */

  unsigned int goodsig : 1;	/* good cryptographic signature */
  unsigned int warnsig : 1;     /* maybe good signature */
  unsigned int badsig : 1;	/* bad cryptographic signature (needed to check encrypted s/mime-signatures) */
#ifdef USE_AUTOCRYPT
  unsigned int is_autocrypt : 1;  /* used to flag autocrypt-decrypted messages
                                   * for replying */
#endif

  unsigned int collapsed : 1;	/* used by recvattach */
  unsigned int attach_qualifies : 1;

} BODY;

/* #3279: AIX defines conflicting struct thread */
typedef struct mutt_thread THREAD;

typedef struct header
{
  unsigned int security : 14;  /* bit 0-10:   flags
                                  bit 11-12: application.
                                  bit 13:    traditional pgp.
                                  see: mutt_crypt.h pgplib.h, smime.h */

  unsigned int mime : 1;    		/* has a MIME-Version header? */
  unsigned int flagged : 1; 		/* marked important? */
  unsigned int tagged : 1;
  unsigned int deleted : 1;
  unsigned int purge : 1;               /* skip trash folder when deleting */
  unsigned int changed : 1;
  unsigned int attach_del : 1; 		/* has an attachment marked for deletion */
  unsigned int old : 1;
  unsigned int read : 1;
  unsigned int expired : 1; 		/* already expired? */
  unsigned int superseded : 1; 		/* got superseded? */
  unsigned int replied : 1;
  unsigned int subject_changed : 1; 	/* used for threading */
  unsigned int threaded : 1;	    	/* used for threading */
  unsigned int display_subject : 1; 	/* used for threading */
  unsigned int recip_valid : 1;  	/* is_recipient is valid */
  unsigned int active : 1;	    	/* message is not to be removed */
  unsigned int trash : 1;		/* message is marked as trashed on disk.
					 * This flag is used by the maildir_trash
					 * option.
					 */

  /* timezone of the sender of this message */
  unsigned int zhours : 5;
  unsigned int zminutes : 6;
  unsigned int zoccident : 1;

  /* bits used for caching when searching */
  unsigned int searched : 1;
  unsigned int matched : 1;

  /* tells whether the attachment count is valid */
  unsigned int attach_valid : 1;

  /* the following are used to support collapsing threads  */
  unsigned int collapsed : 1; 	/* is this message part of a collapsed thread? */
  unsigned int limited : 1;   	/* is this message in a limited view?  */
  size_t num_hidden;            /* number of hidden messages in this view.
                                 * only valid when collapsed is set. */

  short recipient;		/* user_is_recipient()'s return value, cached */

  COLOR_ATTR color; 		/* color-pair to use when displaying in the index */

  time_t date_sent;     	/* time when the message was sent (UTC) */
  time_t received;      	/* time when the message was placed in the mailbox */
  LOFF_T offset;          	/* where in the stream does this message begin? */
  int lines;			/* how many lines in the body of this message? */
  int index;			/* the absolute (unsorted) message number */
  int msgno;			/* number displayed to the user */
  int virtual;			/* virtual message number */
  int score;
  ENVELOPE *env;		/* envelope information */
  BODY *content;		/* list of MIME parts */
  char *path;

  char *tree;           	/* character string to print thread tree */
  THREAD *thread;

  /* Number of qualifying attachments in message, if attach_valid */
  short attach_total;

#ifdef MIXMASTER
  LIST *chain;
#endif

#ifdef USE_POP
  int refno;			/* message number on server */
#endif

#if defined USE_POP || defined USE_IMAP
  void *data;            	/* driver-specific data */
#endif

  char *maildir_flags;		/* unknown maildir flags */
} HEADER;

struct mutt_thread
{
  unsigned int fake_thread : 1;
  unsigned int duplicate_thread : 1;
  unsigned int sort_children : 1;
  unsigned int recalc_aux_key : 1;
  unsigned int recalc_group_key : 1;
  unsigned int check_subject : 1;
  unsigned int visible : 1;
  unsigned int deep : 1;
  unsigned int subtree_visible : 2;
  unsigned int next_subtree_visible : 1;
  THREAD *parent;
  THREAD *child;
  THREAD *next;
  THREAD *prev;
  HEADER *message;
  HEADER *sort_group_key;  /* $sort_thread_groups - for thread roots */
  HEADER *sort_aux_key;    /* $sort_aux - for messages below the root */
};


/* flag to mutt_pattern_comp() */
#define MUTT_FULL_MSG           (1<<0)  /* enable body and header matching */
#define MUTT_PATTERN_DYNAMIC    (1<<1)  /* enable runtime date range evaluation */
#define MUTT_SEND_MODE_SEARCH   (1<<2)  /* allow send-mode body searching */

typedef enum {
  MUTT_MATCH_FULL_ADDRESS = 1
} pattern_exec_flag;

typedef struct group_t
{
  ADDRESS *as;
  RX_LIST *rs;
  char *name;
} group_t;

typedef struct group_context_t
{
  group_t *g;
  struct group_context_t *next;
} group_context_t;

typedef struct pattern_t
{
  short op;
  unsigned int not : 1;
  unsigned int alladdr : 1;
  unsigned int stringmatch : 1;
  unsigned int groupmatch : 1;
  unsigned int ign_case : 1;		/* ignore case for local stringmatch searches */
  unsigned int isalias : 1;
  unsigned int dynamic : 1;  /* evaluate date ranges at run time */
  unsigned int sendmode : 1; /* evaluate searches in send-mode */
  int min;
  int max;
  struct pattern_t *next;
  struct pattern_t *child;		/* arguments to logical op */
  union
  {
    regex_t *rx;
    group_t *g;
    char *str;
  } p;
} pattern_t;

/* This is used when a message is repeatedly pattern matched against.
 * e.g. for color, scoring, hooks.  It caches a few of the potentially slow
 * operations.
 * Each entry has a value of 0 = unset, 1 = false, 2 = true
 */
typedef struct
{
  int list_all;          /* ^~l */
  int list_one;          /*  ~l */
  int sub_all;           /* ^~u */
  int sub_one;           /*  ~u */
  int pers_recip_all;    /* ^~p */
  int pers_recip_one;    /*  ~p */
  int pers_from_all;     /* ^~P */
  int pers_from_one;     /*  ~P */
} pattern_cache_t;

/* ACL Rights */
enum
{
  MUTT_ACL_LOOKUP = 0,
  MUTT_ACL_READ,
  MUTT_ACL_SEEN,
  MUTT_ACL_WRITE,
  MUTT_ACL_INSERT,
  MUTT_ACL_POST,
  MUTT_ACL_CREATE,
  MUTT_ACL_DELMX,
  MUTT_ACL_DELETE,
  MUTT_ACL_EXPUNGE,
  MUTT_ACL_ADMIN,

  RIGHTSMAX
};

struct _context;
struct _message;

/*
 * struct mx_ops - a structure to store operations on a mailbox
 * The following operations are mandatory:
 *  - open
 *  - close
 *  - check
 *
 * Optional operations
 *  - open_new_msg
 */
struct mx_ops
{
  int (*open) (struct _context *);
  int (*open_append) (struct _context *, int flags);
  int (*close) (struct _context *);
  int (*check) (struct _context *ctx, int *index_hint);
  int (*sync) (struct _context *ctx, int *index_hint);
  int (*open_msg) (struct _context *, struct _message *, int msgno, int headers);
  int (*close_msg) (struct _context *, struct _message *);
  int (*commit_msg) (struct _context *, struct _message *);
  int (*open_new_msg) (struct _message *, struct _context *, HEADER *);
  int (*msg_padding_size) (struct _context *);
  int (*save_to_header_cache) (struct _context *, struct header *);
};

typedef struct _context
{
  char *path;
  char *realpath;               /* used for buffy comparison and the sidebar */
  FILE *fp;
  struct timespec atime;
  struct timespec mtime;
  off_t size;
  off_t vsize;
  char *pattern;                /* limit pattern string */
  pattern_t *limit_pattern;     /* compiled limit pattern */
  HEADER **hdrs;
  HEADER *last_tag;		/* last tagged msg. used to link threads */
  THREAD *tree;			/* top of thread tree */
  HASH *id_hash;		/* hash table by msg id */
  HASH *subj_hash;		/* hash table by subject */
  HASH *thread_hash;		/* hash table for threading */
  HASH *label_hash;             /* hash table for x-labels */
  int *v2r;			/* mapping from virtual to real msgno */
  int hdrmax;			/* number of pointers in hdrs */
  int msgcount;			/* number of messages in the mailbox */
  int vcount;			/* the number of virtual messages */
  int tagged;			/* how many messages are tagged? */
  int new;			/* how many new messages? */
  int unread;			/* how many unread messages? */
  int deleted;			/* how many deleted messages */
  int trashed;			/* how many marked as trashed on disk.
                                 * This flag is used by the maildir_trash
                                 * option.
                                 */
  int flagged;			/* how many flagged messages */
  int msgnotreadyet;		/* which msg "new" in pager, -1 if none */

  short magic;			/* mailbox type */

  unsigned char rights[(RIGHTSMAX + 7)/8];	/* ACL bits */

  unsigned int locked : 1;	/* is the mailbox locked? */
  unsigned int changed : 1;	/* mailbox has been modified */
  unsigned int readonly : 1;    /* don't allow changes to the mailbox */
  unsigned int dontwrite : 1;   /* don't write the mailbox on close */
  unsigned int append : 1;	/* mailbox is opened in append mode */
  unsigned int quiet : 1;	/* inhibit status messages? */
  unsigned int collapsed : 1;   /* are all threads collapsed? */
  unsigned int closing : 1;	/* mailbox is being closed */
  unsigned int peekonly : 1;	/* just taking a glance, revert atime */

#ifdef USE_COMPRESSED
  void *compress_info;		/* compressed mbox module private data */
#endif /* USE_COMPRESSED */

  /* driver hooks */
  void *data;			/* driver specific data */
  struct mx_ops *mx_ops;
} CONTEXT;

typedef struct
{
  FILE *fpin;
  FILE *fpout;
  char *prefix;
  int flags;
} STATE;

/* used by enter.c */

typedef struct
{
  wchar_t *wbuf;
  size_t wbuflen;
  size_t lastchar;
  size_t curpos;
  size_t begin;
  int	 tabs;
} ENTER_STATE;

/* flags for the STATE struct */
#define MUTT_DISPLAY       (1<<0) /* output is displayed to the user */
#define MUTT_VERIFY        (1<<1) /* perform signature verification */
#define MUTT_PENDINGPREFIX (1<<2) /* prefix to write, but character must follow */
#define MUTT_WEED          (1<<3) /* weed headers even when not in display mode */
#define MUTT_CHARCONV      (1<<4) /* Do character set conversions */
#define MUTT_PRINTING      (1<<5) /* are we printing? - MUTT_DISPLAY "light" */
#define MUTT_REPLYING      (1<<6) /* are we replying? */
#define MUTT_FORWARDING    (1<<7) /* are we inline forwarding? */
#define MUTT_FIRSTDONE     (1<<8) /* the first attachment has been done */

#define state_set_prefix(s) ((s)->flags |= MUTT_PENDINGPREFIX)
#define state_reset_prefix(s) ((s)->flags &= ~MUTT_PENDINGPREFIX)
#define state_puts(x,y) fputs(x,(y)->fpout)
#define state_putc(x,y) fputc(x,(y)->fpout)

void state_mark_attach (STATE *);
void state_mark_protected_header (STATE *);
void state_attach_puts (const char *, STATE *);
void state_prefix_putc (char, STATE *);
int  state_printf(STATE *, const char *, ...);
int state_putwc (wchar_t, STATE *);
int state_putws (const wchar_t *, STATE *);

/* for attachment counter */
typedef struct
{
  char   *major;
  int     major_int;
  char   *minor;
  regex_t minor_rx;
} ATTACH_MATCH;

/* multibyte character table.
 * Allows for direct access to the individual multibyte characters in a
 * string.  This is used for the Tochars and StChars option types. */
typedef struct
{
  int len;               /* number of characters */
  char **chars;          /* the array of multibyte character strings */
  char *segmented_str;   /* each chars entry points inside this string */
  char *orig_str;
} mbchar_table;

/* flags for count_body_parts() */
#define MUTT_PARTS_TOPLEVEL      (1<<0) /* is the top-level part */
#define MUTT_PARTS_ROOT_MPALT    (1<<1) /* root multipart/alternative */
#define MUTT_PARTS_NONROOT_MPALT (1<<2) /* non-root multipart/alternative */

#include "send.h"
#include "ascii.h"
#include "protos.h"
#include "lib.h"
#include "globals.h"

#endif /*MUTT_H*/
