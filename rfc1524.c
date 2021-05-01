/*
 * Copyright (C) 1996-2000,2003,2012 Michael R. Elkins <me@mutt.org>
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

/*
 * rfc1524 defines a format for the Multimedia Mail Configuration, which
 * is the standard mailcap file format under Unix which specifies what
 * external programs should be used to view/compose/edit multimedia files
 * based on content type.
 *
 * This file contains various functions for implementing a fair subset of
 * rfc1524.
 */

#if HAVE_CONFIG_H
# include "config.h"
#endif

#include "mutt.h"
#include "rfc1524.h"

#include <string.h>
#include <stdlib.h>
#include <ctype.h>

#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <unistd.h>

/* The command semantics include the following:
 * %s is the filename that contains the mail body data
 * %t is the content type, like text/plain
 * %{parameter} is replaced by the parameter value from the content-type field
 * \% is %
 * Unsupported rfc1524 parameters: these would probably require some doing
 * by mutt, and can probably just be done by piping the message to metamail
 * %n is the integer number of sub-parts in the multipart
 * %F is "content-type filename" repeated for each sub-part
 *
 * In addition, this function returns a 0 if the command works on a file,
 * and 1 if the command works on a pipe.
 */
int mutt_rfc1524_expand_command (BODY *a, const char *filename, const char *_type,
                                 BUFFER *command)
{
  const char *cptr;
  int needspipe = TRUE;
  BUFFER *buf = NULL;
  BUFFER *quoted = NULL;
  BUFFER *param = NULL;
  BUFFER *type = NULL;

  buf = mutt_buffer_pool_get ();
  quoted = mutt_buffer_pool_get ();

  cptr = mutt_b2s (command);
  while (*cptr)
  {
    if (*cptr == '\\')
    {
      cptr++;
      if (*cptr)
        mutt_buffer_addch (buf, *cptr++);
    }
    else if (*cptr == '%')
    {
      cptr++;
      if (*cptr == '{')
      {
	const char *_pvalue;

        if (!param)
          param = mutt_buffer_pool_get ();
        else
          mutt_buffer_clear (param);

        /* Copy parameter name into param buffer */
	cptr++;
	while (*cptr && *cptr != '}')
          mutt_buffer_addch (param, *cptr++);

        /* In send mode, use the current charset, since the message hasn't
         * been converted yet.   If noconv is set, then we assume the
         * charset parameter has the correct value instead. */
        if ((ascii_strcasecmp (mutt_b2s (param), "charset") == 0) && a->charset && !a->noconv)
          _pvalue = a->charset;
        else
          _pvalue = mutt_get_parameter (mutt_b2s (param), a->parameter);

        /* Now copy the parameter value into param buffer */
	if (option (OPTMAILCAPSANITIZE))
	  mutt_buffer_sanitize_filename (param, NONULL(_pvalue),
                                         MUTT_SANITIZE_ALLOW_SLASH);
        else
          mutt_buffer_strcpy (param, NONULL(_pvalue));

	mutt_buffer_quote_filename (quoted, mutt_b2s (param));
        mutt_buffer_addstr (buf, mutt_b2s (quoted));
      }
      else if (*cptr == 's' && filename != NULL)
      {
	mutt_buffer_quote_filename (quoted, filename);
        mutt_buffer_addstr (buf, mutt_b2s (quoted));
	needspipe = FALSE;
      }
      else if (*cptr == 't')
      {
        if (!type)
        {
          type = mutt_buffer_pool_get ();
          if (option (OPTMAILCAPSANITIZE))
            mutt_buffer_sanitize_filename (type, _type,
                                           MUTT_SANITIZE_ALLOW_SLASH);
          else
            mutt_buffer_strcpy (type, _type);
        }
	mutt_buffer_quote_filename (quoted, mutt_b2s (type));
        mutt_buffer_addstr (buf, mutt_b2s (quoted));
      }

      if (*cptr)
        cptr++;
    }
    else
      mutt_buffer_addch (buf, *cptr++);
  }
  mutt_buffer_strcpy (command, mutt_b2s (buf));

  mutt_buffer_pool_release (&buf);
  mutt_buffer_pool_release (&quoted);
  mutt_buffer_pool_release (&param);
  mutt_buffer_pool_release (&type);

  return needspipe;
}

/* NUL terminates a rfc 1524 field,
 * returns start of next field or NULL */
static char *get_field (char *s)
{
  char *ch;

  if (!s)
    return NULL;

  while ((ch = strpbrk (s, ";\\")) != NULL)
  {
    if (*ch == '\\')
    {
      s = ch + 1;
      if (*s)
	s++;
    }
    else
    {
      *ch = 0;
      ch = skip_email_wsp(ch + 1);
      break;
    }
  }
  mutt_remove_trailing_ws (s);
  return ch;
}

static int get_field_text (char *field, char **entry,
			   const char *type, const char *filename, int line)
{
  field = mutt_skip_whitespace (field);
  if (*field == '=')
  {
    if (entry)
    {
      field++;
      field = mutt_skip_whitespace (field);
      mutt_str_replace (entry, field);
    }
    return 1;
  }
  else
  {
    mutt_error (_("Improperly formatted entry for type %s in \"%s\" line %d"),
		type, filename, line);
    return 0;
  }
}

static int rfc1524_mailcap_parse (BODY *a,
				  const char *filename,
				  const char *type,
				  rfc1524_entry *entry,
				  int opt)
{
  FILE *fp;
  char *buf = NULL;
  size_t buflen;
  char *ch;
  char *field;
  int found = FALSE;
  int copiousoutput;
  int composecommand;
  int editcommand;
  int printcommand;
  int btlen;
  int line = 0;

  /* rfc1524 mailcap file is of the format:
   * base/type; command; extradefs
   * type can be * for matching all
   * base with no /type is an implicit wild
   * command contains a %s for the filename to pass, default to pipe on stdin
   * extradefs are of the form:
   *  def1="definition"; def2="define \;";
   * line wraps with a \ at the end of the line
   * # for comments
   */

  /* find length of basetype */
  if ((ch = strchr (type, '/')) == NULL)
    return FALSE;
  btlen = ch - type;

  if ((fp = fopen (filename, "r")) != NULL)
  {
    while (!found && (buf = mutt_read_line (buf, &buflen, fp, &line, MUTT_CONT)) != NULL)
    {
      /* ignore comments */
      if (*buf == '#')
	continue;
      dprint (2, (debugfile, "mailcap entry: %s\n", buf));

      /* check type */
      ch = get_field (buf);
      if (ascii_strcasecmp (buf, type) &&
	  (ascii_strncasecmp (buf, type, btlen) ||
	   (buf[btlen] != 0 &&			/* implicit wild */
	    mutt_strcmp (buf + btlen, "/*"))))	/* wildsubtype */
	continue;

      /* next field is the viewcommand */
      field = ch;
      ch = get_field (ch);
      if (entry)
	entry->command = safe_strdup (field);

      /* parse the optional fields */
      found = TRUE;
      copiousoutput = FALSE;
      composecommand = FALSE;
      editcommand = FALSE;
      printcommand = FALSE;

      while (ch)
      {
	field = ch;
	ch = get_field (ch);
	dprint (2, (debugfile, "field: %s\n", field));

	if (!ascii_strcasecmp (field, "needsterminal"))
	{
	  if (entry)
	    entry->needsterminal = TRUE;
	}
	else if (!ascii_strcasecmp (field, "copiousoutput"))
	{
	  copiousoutput = TRUE;
	  if (entry)
	    entry->copiousoutput = TRUE;
	}
	else if (!ascii_strncasecmp (field, "composetyped", 12))
	{
	  /* this compare most occur before compose to match correctly */
	  if (get_field_text (field + 12, entry ? &entry->composetypecommand : NULL,
			      type, filename, line))
	    composecommand = TRUE;
	}
	else if (!ascii_strncasecmp (field, "compose", 7))
	{
	  if (get_field_text (field + 7, entry ? &entry->composecommand : NULL,
			      type, filename, line))
	    composecommand = TRUE;
	}
	else if (!ascii_strncasecmp (field, "print", 5))
	{
	  if (get_field_text (field + 5, entry ? &entry->printcommand : NULL,
			      type, filename, line))
	    printcommand = TRUE;
	}
	else if (!ascii_strncasecmp (field, "edit", 4))
	{
	  if (get_field_text (field + 4, entry ? &entry->editcommand : NULL,
			      type, filename, line))
	    editcommand = TRUE;
	}
	else if (!ascii_strncasecmp (field, "nametemplate", 12))
	{
	  get_field_text (field + 12, entry ? &entry->nametemplate : NULL,
			  type, filename, line);
	}
	else if (!ascii_strncasecmp (field, "x-convert", 9))
	{
	  get_field_text (field + 9, entry ? &entry->convert : NULL,
			  type, filename, line);
	}
	else if (!ascii_strncasecmp (field, "test", 4))
	{
	  /*
	   * This routine executes the given test command to determine
	   * if this is the right entry.
	   */
	  char *test_command = NULL;
          BUFFER *command = NULL;
          BUFFER *afilename = NULL;

	  if (get_field_text (field + 4, &test_command, type, filename, line)
	      && test_command)
	  {
            command = mutt_buffer_pool_get ();
            afilename = mutt_buffer_pool_get ();
            mutt_buffer_strcpy (command, test_command);
            if (option (OPTMAILCAPSANITIZE))
              mutt_buffer_sanitize_filename (afilename, NONULL(a->filename), 0);
            else
              mutt_buffer_strcpy (afilename, NONULL(a->filename));
	    mutt_rfc1524_expand_command (a, mutt_b2s (afilename), type, command);
	    if (mutt_system (mutt_b2s (command)))
	    {
	      /* a non-zero exit code means test failed */
	      found = FALSE;
	    }
	    FREE (&test_command);
            mutt_buffer_pool_release (&command);
            mutt_buffer_pool_release (&afilename);
	  }
	}
      } /* while (ch) */

      if (opt == MUTT_AUTOVIEW)
      {
	if (!copiousoutput)
	  found = FALSE;
      }
      else if (opt == MUTT_COMPOSE)
      {
	if (!composecommand)
	  found = FALSE;
      }
      else if (opt == MUTT_EDIT)
      {
	if (!editcommand)
	  found = FALSE;
      }
      else if (opt == MUTT_PRINT)
      {
	if (!printcommand)
	  found = FALSE;
      }

      if (!found)
      {
	/* reset */
	if (entry)
	{
	  FREE (&entry->command);
	  FREE (&entry->composecommand);
	  FREE (&entry->composetypecommand);
	  FREE (&entry->editcommand);
	  FREE (&entry->printcommand);
	  FREE (&entry->nametemplate);
	  FREE (&entry->convert);
	  entry->needsterminal = 0;
	  entry->copiousoutput = 0;
	}
      }
    } /* while (!found && (buf = mutt_read_line ())) */
    safe_fclose (&fp);
  } /* if ((fp = fopen ())) */
  FREE (&buf);
  return found;
}

rfc1524_entry *rfc1524_new_entry(void)
{
  return (rfc1524_entry *)safe_calloc(1, sizeof(rfc1524_entry));
}

void rfc1524_free_entry(rfc1524_entry **entry)
{
  rfc1524_entry *p;

  if (!entry || !*entry)
    return;

  p = *entry;
  FREE (&p->command);
  FREE (&p->testcommand);
  FREE (&p->composecommand);
  FREE (&p->composetypecommand);
  FREE (&p->editcommand);
  FREE (&p->printcommand);
  FREE (&p->nametemplate);
  FREE (entry);		/* __FREE_CHECKED__ */
}

/*
 * rfc1524_mailcap_lookup attempts to find the given type in the
 * list of mailcap files.  On success, this returns the entry information
 * in *entry, and returns 1.  On failure (not found), returns 0.
 * If entry == NULL just return 1 if the given type is found.
 */
int rfc1524_mailcap_lookup (BODY *a, char *type, size_t typelen, rfc1524_entry *entry, int opt)
{
  BUFFER *path = NULL;
  int found = FALSE;
  char *curr = MailcapPath;

  /* rfc1524 specifies that a path of mailcap files should be searched.
   * joy.  They say
   * $HOME/.mailcap:/etc/mailcap:/usr/etc/mailcap:/usr/local/etc/mailcap, etc
   * and overridden by the MAILCAPS environment variable, and, just to be nice,
   * we'll make it specifiable in .muttrc
   */
  if (!curr)
  {
    /* L10N:
       Mutt is trying to look up a mailcap value, but $mailcap_path is
       empty.  We added a reference to the MAILCAPS environment
       variable as a hint too.

       Because the variable is automatically populated by Mutt, this
       should only occur if the user deliberately runs in their shell:
         export MAILCAPS=

       or deliberately runs inside Mutt or their .muttrc:
         set mailcap_path=""
         -or-
         unset mailcap_path
    */
    mutt_error _("Neither mailcap_path nor MAILCAPS specified");
    return 0;
  }

  mutt_check_lookup_list (a, type, typelen);

  path = mutt_buffer_pool_get ();

  while (!found && *curr)
  {
    mutt_buffer_clear (path);
    while (*curr && *curr != ':')
    {
      mutt_buffer_addch (path, *curr);
      curr++;
    }
    if (*curr)
      curr++;

    if (!mutt_buffer_len (path))
      continue;

    mutt_buffer_expand_path (path);

    dprint(2,(debugfile,"Checking mailcap file: %s\n",mutt_b2s (path)));
    found = rfc1524_mailcap_parse (a, mutt_b2s (path), type, entry, opt);
  }

  mutt_buffer_pool_release (&path);

  if (entry && !found)
    mutt_error (_("mailcap entry for type %s not found"), type);

  return found;
}


/* This routine will create a _temporary_ filename, matching the
 * name template if given.
 *
 * Please note that only the last path element of the
 * template and/or the old file name will be used for the
 * comparison and the temporary file name.
 */
void mutt_rfc1524_expand_filename (const char *nametemplate,
                                   const char *oldfile,
                                   BUFFER *newfile)
{
  int i, j, k, ps;
  char *s;
  short lmatch = 0, rmatch = 0;
  BUFFER *left = NULL;
  BUFFER *right = NULL;

  mutt_buffer_clear (newfile);

  /* first, ignore leading path components.
   */

  if (nametemplate && (s = strrchr (nametemplate, '/')))
    nametemplate = s + 1;

  if (oldfile && (s = strrchr (oldfile, '/')))
    oldfile = s + 1;

  if (!nametemplate)
  {
    if (oldfile)
      mutt_buffer_strcpy (newfile, oldfile);
  }
  else if (!oldfile)
  {
    mutt_expand_fmt (newfile, nametemplate, "mutt");
  }
  else /* oldfile && nametemplate */
  {

    /* first, compare everything left from the "%s"
     * (if there is one).
     */

    lmatch = 1; ps = 0;
    for (i = 0; nametemplate[i]; i++)
    {
      if (nametemplate[i] == '%' && nametemplate[i+1] == 's')
      {
	ps = 1;
	break;
      }

      /* note that the following will _not_ read beyond oldfile's end. */

      if (lmatch && nametemplate[i] != oldfile[i])
	lmatch = 0;
    }

    if (ps)
    {

      /* If we had a "%s", check the rest. */

      /* now, for the right part: compare everything right from
       * the "%s" to the final part of oldfile.
       *
       * The logic here is as follows:
       *
       * - We start reading from the end.
       * - There must be a match _right_ from the "%s",
       *   thus the i + 2.
       * - If there was a left hand match, this stuff
       *   must not be counted again.  That's done by the
       *   condition (j >= (lmatch ? i : 0)).
       */

      rmatch = 1;

      for (j = mutt_strlen(oldfile) - 1, k = mutt_strlen(nametemplate) - 1 ;
           j >= (lmatch ? i : 0) && k >= i + 2;
           j--, k--)
      {
	if (nametemplate[k] != oldfile[j])
	{
	  rmatch = 0;
	  break;
	}
      }

      /* Now, check if we had a full match. */

      if (k >= i + 2)
	rmatch = 0;

      left = mutt_buffer_pool_get ();
      right = mutt_buffer_pool_get ();

      if (!lmatch)
        mutt_buffer_strcpy_n (left, nametemplate, i);
      if (!rmatch)
        mutt_buffer_strcpy (right, nametemplate + i + 2);
      mutt_buffer_printf (newfile, "%s%s%s", mutt_b2s (left), oldfile, mutt_b2s (right));

      mutt_buffer_pool_release (&left);
      mutt_buffer_pool_release (&right);
    }
    else
    {
      /* no "%s" in the name template. */
      mutt_buffer_strcpy (newfile, nametemplate);
    }
  }

  mutt_adv_mktemp (newfile);
}

/* If rfc1524_expand_command() is used on a recv'd message, then
 * the filename doesn't exist yet, but if its used while sending a message,
 * then we need to rename the existing file.
 *
 * This function returns 0 on successful move, 1 on old file doesn't exist,
 * 2 on new file already exists, and 3 on other failure.
 */

/* note on access(2) use: No dangling symlink problems here due to
 * safe_fopen().
 */

int mutt_rename_file (const char *oldfile, const char *newfile)
{
  FILE *ofp, *nfp;

  if (access (oldfile, F_OK) != 0)
    return 1;
  if (access (newfile, F_OK) == 0)
    return 2;
  if ((ofp = fopen (oldfile,"r")) == NULL)
    return 3;
  if ((nfp = safe_fopen (newfile,"w")) == NULL)
  {
    safe_fclose (&ofp);
    return 3;
  }
  mutt_copy_stream (ofp,nfp);
  safe_fclose (&nfp);
  safe_fclose (&ofp);
  mutt_unlink (oldfile);
  return 0;
}
