/* $Id: ar_fns.c,v 1.9 2005/12/25 20:53:01 rockyb Exp $
Interface to `ar' archives for GNU Make.
Copyright (C) 1988, 1989, 1990, 1991, 1992, 1993, 1997,
2002, 2004, 2005 Free Software Foundation, Inc.
This file is part of GNU Make.

GNU Make is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2, or (at your option)
any later version.

GNU Make is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with GNU Make; see the file COPYING.  If not, write to
the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
Boston, MA 02111-1307, USA.  */

#include "types.h"

#ifndef	NO_ARCHIVES

#include "ar_fns.h"
#include "arscan.h"
#include "dir_fns.h"
#include "misc.h"
#include "print.h"
#include "remake.h"
#include <fnmatch.h>

#ifdef HAVE_ALLOCA_H
#include <alloca.h>
#endif

/*! Return nonzero if NAME is an archive-member reference, zero if not.
   An archive-member reference is a name like `lib(member)'.  If a
   name like `lib((entry))' is used, a fatal error is signaled at the
   attempt to use this unsupported feature.
*/
bool
ar_name (char *psz_name)
{
  char *p = strchr (psz_name, '(');
  char *end;

  if (p == 0 || p == psz_name)
    return false;

  end = p + strlen (p) - 1;
  if (*end != ')')
    return false;

  if (p[1] == '(' && end[-1] == ')')
    fatal (NILF, _("attempt to use unsupported feature: `%s'"), psz_name);

  return true;
}

/*! 
   Parse the archive-member reference into the archive and
   member names.  

   @param psz_name archive-member reference to look up.
   @param arname_p place where the malloc'd archive name if it is non-nil
   @param memname_p place to put malloc'd member name if it is non-nil
*/
void
ar_parse_name (char *psz_name, char **ppsz_arname, char **ppsz_memname)
{
  char *p = strchr (psz_name, '('), *end = psz_name + strlen (psz_name) - 1;

  if (ppsz_arname)
    *ppsz_arname = savestring (psz_name, p - psz_name);

  if (ppsz_memname)
    *ppsz_memname = savestring (p + 1, end - (p + 1));
}

static long int 
ar_member_date_1 (int desc, char *mem, int truncated, long int hdrpos,
		  long int datapos, long int size, long int date, int uid, 
		  int gid, int mode, char *name);

/* Return the modtime of NAME.  */

time_t
ar_member_date (char *name)
{
  char *arname;
  int arname_used = 0;
  char *memname;
  long int val;

  ar_parse_name (name, &arname, &memname);

  /* Make sure we know the modtime of the archive itself because we are
     likely to be called just before commands to remake a member are run,
     and they will change the archive itself.

     But we must be careful not to enter_file the archive itself if it does
     not exist, because pattern_search assumes that files found in the data
     base exist or can be made.  */
  {
    file_t *arfile;
    arfile = lookup_file (arname);
    if (arfile == 0 && file_exists_p (arname))
      {
	arfile = enter_file (arname, NILF);
	arname_used = 1;
      }

    if (arfile != 0)
      (void) f_mtime (arfile, false);
  }

  val = ar_scan (arname, ar_member_date_1, (long int) memname);

  if (!arname_used)
    free (arname);
  free (memname);

  return (val <= 0 ? (time_t) -1 : (time_t) val);
}

/* This function is called by `ar_scan' to find which member to look at.  */

/* ARGSUSED */
static long int
ar_member_date_1 (int desc UNUSED, char *mem, int truncated,
		  long int hdrpos UNUSED, long int datapos UNUSED,
                  long int size UNUSED, long int date,
                  int uid UNUSED, int gid UNUSED, int mode UNUSED, char *name)
{
  return ar_name_equal (name, mem, truncated) ? date : 0;
}

/*! Set the archive-member modtime to now.  
  @param psz_name member modtime name to set
  @return 0 if things went okay 1 if not.
*/
int
ar_touch (char *psz_name)
{
  char *arname, *memname;
  int arname_used = 0;
  int val;

  ar_parse_name (psz_name, &arname, &memname);

  /* Make sure we know the modtime of the archive itself before we
     touch the member, since this will change the archive itself.  */
  {
    file_t *arfile;
    arfile = lookup_file (arname);
    if (arfile == 0)
      {
	arfile = enter_file (arname, NILF);
	arname_used = 1;
      }

    (void) f_mtime (arfile, false);
  }

  val = 1;
  switch (ar_member_touch (arname, memname))
    {
    case -1:
      error (NILF, _("touch: Archive `%s' does not exist"), arname);
      break;
    case -2:
      error (NILF, _("touch: `%s' is not a valid archive"), arname);
      break;
    case -3:
      perror_with_name ("touch: ", arname);
      break;
    case 1:
      error (NILF,
             _("touch: Member `%s' does not exist in `%s'"), memname, arname);
      break;
    case 0:
      val = 0;
      break;
    default:
      error (NILF,
             _("touch: Bad return code from ar_member_touch on `%s'"), 
	     psz_name);
    }

  if (!arname_used)
    free (arname);
  free (memname);

  return val;
}

/* State of an `ar_glob' run, passed to `ar_glob_match'.  */

typedef struct ar_glob_state
  {
    char *arname;
    char *pattern;
    unsigned int size;
    struct nameseq *chain;
    unsigned int n;
  } ar_glob_state_t;

/* This function is called by `ar_scan' to match one archive
   element against the pattern in STATE.  */

static long int
ar_glob_match (int desc UNUSED, char *mem, int truncated UNUSED,
	       long int hdrpos UNUSED, long int datapos UNUSED,
               long int size UNUSED, long int date UNUSED, int uid UNUSED,
               int gid UNUSED, int mode UNUSED, ar_glob_state_t *state)
{
  if (fnmatch (state->pattern, mem, FNM_PATHNAME|FNM_PERIOD) == 0)
    {
      /* We have a match.  Add it to the chain.  */
      struct nameseq *new = (struct nameseq *) xmalloc (state->size);
      new->name = concat (state->arname, mem, ")");
      new->next = state->chain;
      state->chain = new;
      ++state->n;
    }

  return 0L;
}

/* Return nonzero if PATTERN contains any metacharacters.
   Metacharacters can be quoted with backslashes if QUOTE is nonzero.  */
int
glob_pattern_p (const char *pattern, int quote)
{
  const char *p;
  int open = 0;

  for (p = pattern; *p != '\0'; ++p)
    switch (*p)
      {
      case '?':
      case '*':
	return 1;

      case '\\':
	if (quote)
	  ++p;
	break;

      case '[':
	open = 1;
	break;

      case ']':
	if (open)
	  return 1;
	break;
      }

  return 0;
}

/* Glob for MEMBER_PATTERN in archive ARNAME.
   Return a malloc'd chain of matching elements (or nil if none).  */

struct nameseq *
ar_glob (char *arname, char *member_pattern, unsigned int size)
{
  ar_glob_state_t state;
  char **names;
  struct nameseq *n;
  unsigned int i;

  if (! glob_pattern_p (member_pattern, 1))
    return 0;

  /* Scan the archive for matches.
     ar_glob_match will accumulate them in STATE.chain.  */
  i = strlen (arname);
  state.arname = (char *) alloca (i + 2);
  memmove (state.arname, arname, i);
  state.arname[i] = '(';
  state.arname[i + 1] = '\0';
  state.pattern = member_pattern;
  state.size = size;
  state.chain = 0;
  state.n = 0;
  (void) ar_scan (arname, ar_glob_match, (long int) &state);

  if (state.chain == 0)
    return 0;

  /* Now put the names into a vector for sorting.  */
  names = (char **) alloca (state.n * sizeof (char *));
  i = 0;
  for (n = state.chain; n != 0; n = n->next)
    names[i++] = n->name;

  /* Sort them alphabetically.  */
  qsort ((char *) names, i, sizeof (*names), alpha_compare);

  /* Put them back into the chain in the sorted order.  */
  i = 0;
  for (n = state.chain; n != 0; n = n->next)
    n->name = names[i++];

  return state.chain;
}

#endif	/* Not NO_ARCHIVES.  */