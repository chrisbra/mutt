/*
 * Copyright (C) 1996-2000,2007,2010,2012 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 1999-2005,2007 Thomas Roessler <roessler@does-not-exist.org>
 *
 *     This program is free software; you can redistribute it
 *     and/or modify it under the terms of the GNU General Public
 *     License as published by the Free Software Foundation; either
 *     version 2 of the License, or (at your option) any later
 *     version.
 *
 *     This program is distributed in the hope that it will be
 *     useful, but WITHOUT ANY WARRANTY; without even the implied
 *     warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *     PURPOSE.  See the GNU General Public License for more
 *     details.
 *
 *     You should have received a copy of the GNU General Public
 *     License along with this program; if not, write to the Free
 *     Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 *     Boston, MA  02110-1301, USA.
 */

/* mutt functions which are generally useful. */

#ifndef _LIB_H
# define _LIB_H

# include <stdio.h>
# include <string.h>
# include <unistd.h> /* needed for SEEK_SET */
# include <sys/types.h>
# include <sys/stat.h>
# include <time.h>
# include <limits.h>
# include <stdarg.h>
# include <signal.h>

# ifndef _POSIX_PATH_MAX
#  include <limits.h>
# endif

# ifdef ENABLE_NLS
#  include "gettext.h"
# define _(a) (gettext (a))
#  ifdef gettext_noop
#   define N_(a) gettext_noop (a)
#  else
#   define N_(a) (a)
#  endif
# else
#  define _(a) (a)
#  define N_(a) a
# endif

# define TRUE 1
# define FALSE 0

# define HUGE_STRING        8192
# define LONG_STRING        1024
# define STRING             256
# define SHORT_STRING       128

/*
 * Create a format string to be used with scanf.
 * To use it, write, for instance, MUTT_FORMAT(HUGE_STRING).
 *
 * See K&R 2nd ed, p. 231 for an explanation.
 */
# define _MUTT_FORMAT_2(a,b)	"%" a  b
# define _MUTT_FORMAT_1(a, b)	_MUTT_FORMAT_2(#a, b)
# define MUTT_FORMAT(a)		_MUTT_FORMAT_1(a, "s")
# define MUTT_FORMAT2(a,b)	_MUTT_FORMAT_1(a, b)


# define FREE(x) safe_free(x)
# define NONULL(x) x?x:""
# define ISSPACE(c) isspace((unsigned char)c)

#ifdef HAVE_MEMCCPY
# define strfcpy(A,B,C) memccpy(A,B,0,(C)-1), *((A)+(C)-1)=0
#else
/* Note it would be technically more correct to strncpy with length
 * (C)-1, as above.  But this tickles more compiler warnings.
 */
# define strfcpy(A,B,C) strncpy(A,B,C), *((A)+(C)-1)=0
#endif

# undef MAX
# undef MIN
# define MAX(a,b) ((a) < (b) ? (b) : (a))
# define MIN(a,b) ((a) < (b) ? (a) : (b))

#define mutt_numeric_cmp(a,b) ((a) < (b) ? -1 : ((a) > (b) ? 1 : 0))

/* Use this with care.  If the compiler can't see the array
 * definition, it obviously won't produce a correct result. */
#define mutt_array_size(x)  (sizeof (x) / sizeof ((x)[0]))

/* For mutt_format_string() justifications */
/* Making left 0 and center -1 is of course completely nonsensical, but
 * it retains compatibility for any patches that call mutt_format_string.
 * Once patches are updated to use FMT_*, these can be made sane. */
#define FMT_LEFT	0
#define FMT_RIGHT	1
#define FMT_CENTER	-1

#define FOREVER while (1)

/* this macro must check for *c == 0 since isspace(0) has unreliable behavior
   on some systems */
# define SKIPWS(c) while (*(c) && isspace ((unsigned char) *(c))) c++;

#define EMAIL_WSP " \t\r\n"

/* skip over WSP as defined by RFC5322.  This is used primarily for parsing
 * header fields. */

static inline char *skip_email_wsp(const char *s)
{
  if (s)
    return (char *)(s + strspn(s, EMAIL_WSP));
  return (char *)s;
}

static inline int is_email_wsp(char c)
{
  return c && (strchr(EMAIL_WSP, c) != NULL);
}


/*
 * These functions aren't defined in lib.c, but
 * they are used there.
 *
 * A non-mutt "implementation" (ahem) can be found in extlib.c.
 */


# ifndef _EXTLIB_C
extern void (*mutt_error) (const char *, ...);
# endif

# ifdef _LIB_C
#  define MUTT_LIB_WHERE
#  define MUTT_LIB_INITVAL(x) = x
# else
#  define MUTT_LIB_WHERE extern
#  define MUTT_LIB_INITVAL(x)
# endif

void mutt_exit (int);


# ifdef DEBUG

MUTT_LIB_WHERE FILE *debugfile;
MUTT_LIB_WHERE int debuglevel;

void mutt_debug (FILE *, const char *, ...);
void mutt_debug_f (const char *, const int, const char *, const char *, ...);

#  define dprint(N,X) do { if (debuglevel>=N && debugfile) mutt_debug X; } while (0)

/* __func__ is a C99 provision, but we now require C99 so it's safe */
#  define dprintf(N, ...) do { if (debuglevel >= (N)) mutt_debug_f (__FILE__, __LINE__, __func__, __VA_ARGS__); } while (0)

# else

#  define dprint(N,X) do { } while (0)
#  define dprintf(N, ...) do { } while (0)

# endif


/* Exit values used in send_msg() */
#define S_ERR 127
#define S_BKG 126

/* Flags for mutt_read_line() */
#define MUTT_CONT		(1<<0)		/* \-continuation */
#define MUTT_EOL		(1<<1)		/* don't strip \n/\r\n */

/* Flags for mutt_sanitize_filename() and mutt_buffer_sanitize_filename() */
#define MUTT_SANITIZE_ALLOW_SLASH   (1<<0)
#define MUTT_SANITIZE_ALLOW_8BIT    (1<<1)

/* The actual library functions. */

char *mutt_concat_path (char *, const char *, const char *, size_t);
char *mutt_read_line (char *, size_t *, FILE *, int *, int);
char *mutt_skip_whitespace (char *);
char *mutt_strlower (char *);
char *mutt_substrcpy (char *, const char *, const char *, size_t);
char *mutt_substrdup (const char *, const char *);
char *safe_strcat (char *, size_t, const char *);
char *safe_strncat (char *, size_t, const char *, size_t);
char *safe_strdup (const char *);

/* mutt_atoX() flags:
 *
 * Without the flag, the function will return -1, but the dst parameter
 * will still be set to 0. */
#define MUTT_ATOI_ALLOW_EMPTY     (1<<0)  /* allow NULL or "" */
#define MUTT_ATOI_ALLOW_TRAILING  (1<<1)  /* allow values after the number */

/* strtol() wrappers with range checking; they return
 * 	 0 success
 * 	-1 format error
 * 	-2 out of range
 * the dst pointer may be NULL to test only without conversion
 */
int mutt_atos (const char *, short *, int);
int mutt_atoi (const char *, int *, int);
int mutt_atol (const char *, long *, int);
int mutt_atoll (const char *, long long *, int);
int mutt_atoui (const char *, unsigned int *, int);
int mutt_atoul (const char *, unsigned long *, int);
int mutt_atoull (const char *, unsigned long long *, int);

const char *mutt_stristr (const char *, const char *);
const char *mutt_basename (const char *);

int compare_stat (struct stat *, struct stat *);
int mutt_copy_stream (FILE *, FILE *);
int mutt_copy_bytes (FILE *, FILE *, size_t);
int mutt_strcasecmp (const char *, const char *);
int mutt_strcmp (const char *, const char *);
int mutt_strncasecmp (const char *, const char *, size_t);
int mutt_strncmp (const char *, const char *, size_t);
int mutt_strcoll (const char *, const char *);
int safe_asprintf (char **, const char *, ...);
int safe_rename (const char *, const char *);
int safe_fclose (FILE **);
int safe_fsync_close (FILE **);

size_t mutt_strlen (const char *);

void *safe_calloc (size_t, size_t);
void *safe_malloc (size_t);
void mutt_nocurses_error (const char *, ...);
void mutt_remove_trailing_ws (char *);
void mutt_sanitize_filename (char *, int flags);
void mutt_str_replace (char **p, const char *s);
int mutt_mkdir (char *path, mode_t mode);
void mutt_str_adjust (char **p);
void mutt_unlink (const char *);
void safe_free (void *);
void safe_realloc (void *, size_t);

const char *mutt_strsysexit(int e);
#endif
