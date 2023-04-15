/*
 * Copyright (C) 1996-2002 Michael R. Elkins <me@mutt.org>
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
#include "sort.h"
#include "mailbox.h"

#include <string.h>
#include <ctype.h>

#define VISIBLE(hdr, ctx) (hdr->virtual >= 0 || (hdr->collapsed && (!ctx->pattern || hdr->limited)))

/* determine whether a is a descendant of b */
static int is_descendant (THREAD *a, THREAD *b)
{
  while (a)
  {
    if (a == b)
      return (1);
    a = a->parent;
  }
  return (0);
}

/* Determines whether to display a message's subject. */
static int need_display_subject (CONTEXT *ctx, HEADER *hdr)
{
  THREAD *tmp, *tree = hdr->thread;

  /* if the user disabled subject hiding, display it */
  if (!option (OPTHIDETHREADSUBJECT))
    return (1);

  /* if our subject is different from our parent's, display it */
  if (hdr->subject_changed)
    return (1);

  /* if our subject is different from that of our closest previously displayed
   * sibling, display the subject */
  for (tmp = tree->prev; tmp; tmp = tmp->prev)
  {
    hdr = tmp->message;
    if (hdr && VISIBLE (hdr, ctx))
    {
      if (hdr->subject_changed)
	return (1);
      else
	break;
    }
  }

  /* if there is a parent-to-child subject change anywhere between us and our
   * closest displayed ancestor, display the subject */
  for (tmp = tree->parent; tmp; tmp = tmp->parent)
  {
    hdr = tmp->message;
    if (hdr)
    {
      if (VISIBLE (hdr, ctx))
	return (0);
      else if (hdr->subject_changed)
	return (1);
    }
  }

  /* if we have no visible parent or previous sibling, display the subject */
  return (1);
}

static void linearize_tree (CONTEXT *ctx)
{
  THREAD *tree = ctx->tree;
  HEADER **array = ctx->hdrs + (Sort & SORT_REVERSE ? ctx->msgcount - 1 : 0);

  while (tree)
  {
    while (!tree->message)
      tree = tree->child;

    *array = tree->message;
    array += Sort & SORT_REVERSE ? -1 : 1;

    if (tree->child)
      tree = tree->child;
    else
    {
      while (tree)
      {
	if (tree->next)
	{
	  tree = tree->next;
	  break;
	}
	else
	  tree = tree->parent;
      }
    }
  }
}

/* this calculates whether a node is the root of a subtree that has visible
 * nodes, whether a node itself is visible, whether, if invisible, it has
 * depth anyway, and whether any of its later siblings are roots of visible
 * subtrees.  while it's at it, it frees the old thread display, so we can
 * skip parts of the tree in mutt_draw_tree() if we've decided here that we
 * don't care about them any more.
 */
static void calculate_visibility (CONTEXT *ctx, int *max_depth)
{
  THREAD *tmp, *tree = ctx->tree;
  int hide_top_missing = option (OPTHIDETOPMISSING) && !option (OPTHIDEMISSING);
  int hide_top_limited = option (OPTHIDETOPLIMITED) && !option (OPTHIDELIMITED);
  int depth = 0;

  /* we walk each level backwards to make it easier to compute next_subtree_visible */
  while (tree->next)
    tree = tree->next;
  *max_depth = 0;

  FOREVER
  {
    if (depth > *max_depth)
      *max_depth = depth;

    tree->subtree_visible = 0;
    if (tree->message)
    {
      FREE (&tree->message->tree);
      if (VISIBLE (tree->message, ctx))
      {
	tree->deep = 1;
	tree->visible = 1;
	tree->message->display_subject = need_display_subject (ctx, tree->message);
	for (tmp = tree; tmp; tmp = tmp->parent)
	{
	  if (tmp->subtree_visible)
	  {
	    tmp->deep = 1;
	    tmp->subtree_visible = 2;
	    break;
	  }
	  else
	    tmp->subtree_visible = 1;
	}
      }
      else
      {
	tree->visible = 0;
	tree->deep = !option (OPTHIDELIMITED);
      }
    }
    else
    {
      tree->visible = 0;
      tree->deep = !option (OPTHIDEMISSING);
    }
    tree->next_subtree_visible = tree->next && (tree->next->next_subtree_visible
						|| tree->next->subtree_visible);
    if (tree->child)
    {
      depth++;
      tree = tree->child;
      while (tree->next)
	tree = tree->next;
    }
    else if (tree->prev)
      tree = tree->prev;
    else
    {
      while (tree && !tree->prev)
      {
	depth--;
	tree = tree->parent;
      }
      if (!tree)
	break;
      else
	tree = tree->prev;
    }
  }

  /* now fix up for the OPTHIDETOP* options if necessary */
  if (hide_top_limited || hide_top_missing)
  {
    tree = ctx->tree;
    FOREVER
    {
      if (!tree->visible && tree->deep && tree->subtree_visible < 2
	  && ((tree->message && hide_top_limited) || (!tree->message && hide_top_missing)))
	tree->deep = 0;
      if (!tree->deep && tree->child && tree->subtree_visible)
	tree = tree->child;
      else if (tree->next)
	tree = tree->next;
      else
      {
	while (tree && !tree->next)
	  tree = tree->parent;
	if (!tree)
	  break;
	else
	  tree = tree->next;
      }
    }
  }
}

/* Since the graphics characters have a value >255, I have to resort to
 * using escape sequences to pass the information to print_enriched_string().
 * These are the macros MUTT_TREE_* defined in mutt.h.
 *
 * ncurses should automatically use the default ASCII characters instead of
 * graphics chars on terminals which don't support them (see the man page
 * for curs_addch).
 */
void mutt_draw_tree (CONTEXT *ctx)
{
  char *pfx = NULL, *mypfx = NULL, *arrow = NULL, *myarrow = NULL, *new_tree;
  char corner = (Sort & SORT_REVERSE) ? MUTT_TREE_ULCORNER : MUTT_TREE_LLCORNER;
  char vtee = (Sort & SORT_REVERSE) ? MUTT_TREE_BTEE : MUTT_TREE_TTEE;
  int depth = 0, start_depth = 0, max_depth = 0, width = option (OPTNARROWTREE) ? 1 : 2;
  THREAD *nextdisp = NULL, *pseudo = NULL, *parent = NULL, *tree = ctx->tree;

  /* Do the visibility calculations and free the old thread chars.
   * From now on we can simply ignore invisible subtrees
   */
  calculate_visibility (ctx, &max_depth);
  pfx = safe_malloc (width * max_depth + 2);
  arrow = safe_malloc (width * max_depth + 2);
  while (tree)
  {
    if (depth)
    {
      myarrow = arrow + (depth - start_depth - (start_depth ? 0 : 1)) * width;
      if (depth && start_depth == depth)
	myarrow[0] = nextdisp ? MUTT_TREE_LTEE : corner;
      else if (parent->message && !option (OPTHIDELIMITED))
	myarrow[0] = MUTT_TREE_HIDDEN;
      else if (!parent->message && !option (OPTHIDEMISSING))
	myarrow[0] = MUTT_TREE_MISSING;
      else
	myarrow[0] = vtee;
      if (width == 2)
	myarrow[1] = pseudo ?  MUTT_TREE_STAR
          : (tree->duplicate_thread ? MUTT_TREE_EQUALS : MUTT_TREE_HLINE);
      if (tree->visible)
      {
	myarrow[width] = MUTT_TREE_RARROW;
	myarrow[width + 1] = 0;
	new_tree = safe_malloc ((2 + depth * width));
	if (start_depth > 1)
	{
	  strncpy (new_tree, pfx, (start_depth - 1) * width);
	  strfcpy (new_tree + (start_depth - 1) * width,
		   arrow, (1 + depth - start_depth) * width + 2);
	}
	else
	  strfcpy (new_tree, arrow, 2 + depth * width);
	tree->message->tree = new_tree;
      }
    }
    if (tree->child && depth)
    {
      mypfx = pfx + (depth - 1) * width;
      mypfx[0] = nextdisp ? MUTT_TREE_VLINE : MUTT_TREE_SPACE;
      if (width == 2)
	mypfx[1] = MUTT_TREE_SPACE;
    }
    parent = tree;
    nextdisp = NULL;
    pseudo = NULL;
    do
    {
      if (tree->child && tree->subtree_visible)
      {
	if (tree->deep)
	  depth++;
	if (tree->visible)
	  start_depth = depth;
	tree = tree->child;

	/* we do this here because we need to make sure that the first child thread
	 * of the old tree that we deal with is actually displayed if any are,
	 * or we might set the parent variable wrong while going through it. */
	while (!tree->subtree_visible && tree->next)
	  tree = tree->next;
      }
      else
      {
	while (!tree->next && tree->parent)
	{
	  if (tree == pseudo)
	    pseudo = NULL;
	  if (tree == nextdisp)
	    nextdisp = NULL;
	  if (tree->visible)
	    start_depth = depth;
	  tree = tree->parent;
	  if (tree->deep)
	  {
	    if (start_depth == depth)
	      start_depth--;
	    depth--;
	  }
	}
	if (tree == pseudo)
	  pseudo = NULL;
	if (tree == nextdisp)
	  nextdisp = NULL;
	if (tree->visible)
	  start_depth = depth;
	tree = tree->next;
	if (!tree)
	  break;
      }
      if (!pseudo && tree->fake_thread)
	pseudo = tree;
      if (!nextdisp && tree->next_subtree_visible)
	nextdisp = tree;
    }
    while (!tree->deep);
  }

  FREE (&pfx);
  FREE (&arrow);
}

/* since we may be trying to attach as a pseudo-thread a THREAD that
 * has no message, we have to make a list of all the subjects of its
 * most immediate existing descendants.  we also note the earliest
 * date on any of the parents and put it in *dateptr. */
static LIST *make_subject_list (THREAD *cur, time_t *dateptr)
{
  THREAD *start = cur;
  ENVELOPE *env;
  time_t thisdate;
  LIST *curlist, *oldlist, *newlist, *subjects = NULL;
  int rc = 0;

  FOREVER
  {
    while (!cur->message)
      cur = cur->child;

    if (dateptr)
    {
      thisdate = option (OPTTHREADRECEIVED)
	? cur->message->received : cur->message->date_sent;
      if (!*dateptr || thisdate < *dateptr)
	*dateptr = thisdate;
    }

    env = cur->message->env;
    if (env->real_subj &&
	((env->real_subj != env->subject) || (!option (OPTSORTRE))))
    {
      for (curlist = subjects, oldlist = NULL;
	   curlist; oldlist = curlist, curlist = curlist->next)
      {
	rc = mutt_strcmp (env->real_subj, curlist->data);
	if (rc >= 0)
	  break;
      }
      if (!curlist || rc > 0)
      {
	newlist = safe_calloc (1, sizeof (LIST));
	newlist->data = env->real_subj;
	if (oldlist)
	{
	  newlist->next = oldlist->next;
	  oldlist->next = newlist;
	}
	else
	{
	  newlist->next = subjects;
	  subjects = newlist;
	}
      }
    }

    while (!cur->next && cur != start)
    {
      cur = cur->parent;
    }
    if (cur == start)
      break;
    cur = cur->next;
  }

  return (subjects);
}

/* find the best possible match for a parent message based upon subject.
 * if there are multiple matches, the one which was sent the latest, but
 * before the current message, is used.
 */
static THREAD *find_subject (CONTEXT *ctx, THREAD *cur)
{
  struct hash_elem *ptr;
  THREAD *tmp, *last = NULL;
  LIST *subjects = NULL, *oldlist;
  time_t date = 0;

  subjects = make_subject_list (cur, &date);

  while (subjects)
  {
    for (ptr = hash_find_bucket (ctx->subj_hash, subjects->data); ptr; ptr = ptr->next)
    {
      tmp = ((HEADER *) ptr->data)->thread;
      if (tmp != cur &&			   /* don't match the same message */
	  !tmp->fake_thread &&		   /* don't match pseudo threads */
	  tmp->message->subject_changed && /* only match interesting replies */
	  !is_descendant (tmp, cur) &&	   /* don't match in the same thread */
	  (date >= (option (OPTTHREADRECEIVED) ?
		    tmp->message->received :
		    tmp->message->date_sent)) &&
	  (!last ||
	   (option (OPTTHREADRECEIVED) ?
	    (last->message->received < tmp->message->received) :
	    (last->message->date_sent < tmp->message->date_sent))) &&
	  tmp->message->env->real_subj &&
	  mutt_strcmp (subjects->data, tmp->message->env->real_subj) == 0)
	last = tmp; /* best match so far */
    }

    oldlist = subjects;
    subjects = subjects->next;
    FREE (&oldlist);
  }
  return (last);
}

/* remove cur and its descendants from their current location.
 * also make sure ancestors of cur no longer are sorted by the
 * fact that cur is their descendant. */
static void unlink_message (THREAD **old, THREAD *cur)
{
  THREAD *tmp;

  if (cur->prev)
    cur->prev->next = cur->next;
  else
    *old = cur->next;

  if (cur->next)
    cur->next->prev = cur->prev;

  if (cur->sort_aux_key)
  {
    for (tmp = cur->parent; tmp && tmp->sort_aux_key == cur->sort_aux_key;
	 tmp = tmp->parent)
      tmp->sort_aux_key = NULL;
  }
  if (cur->sort_group_key)
  {
    for (tmp = cur->parent; tmp && tmp->sort_group_key == cur->sort_group_key;
	 tmp = tmp->parent)
      tmp->sort_group_key = NULL;
  }
}

/* add cur as a prior sibling of *new, with parent newparent */
static void insert_message (THREAD **new, THREAD *newparent, THREAD *cur)
{
  if (*new)
    (*new)->prev = cur;

  cur->parent = newparent;
  cur->next = *new;
  cur->prev = NULL;
  *new = cur;

  if (newparent)
  {
    newparent->recalc_aux_key = 1;
    newparent->recalc_group_key = 1;
    newparent->sort_children = 1;
  }
}

/* thread by subject things that didn't get threaded by message-id */
static void pseudo_threads (CONTEXT *ctx)
{
  THREAD *tree = ctx->tree, *top = tree;
  THREAD *tmp, *cur, *parent, *curchild, *nextchild;

  if (!ctx->subj_hash)
    ctx->subj_hash = mutt_make_subj_hash (ctx);

  while (tree)
  {
    cur = tree;
    tree = tree->next;
    if ((parent = find_subject (ctx, cur)) != NULL)
    {
      cur->fake_thread = 1;
      unlink_message (&top, cur);
      insert_message (&parent->child, parent, cur);
      tmp = cur;
      FOREVER
      {
	while (!tmp->message)
	  tmp = tmp->child;

	/* if the message we're attaching has pseudo-children, they
	 * need to be attached to its parent, so move them up a level.
	 * but only do this if they have the same real subject as the
	 * parent, since otherwise they rightly belong to the message
	 * we're attaching. */
	if (tmp == cur
	    || !mutt_strcmp (tmp->message->env->real_subj,
			     parent->message->env->real_subj))
	{
	  tmp->message->subject_changed = 0;

	  for (curchild = tmp->child; curchild; )
	  {
	    nextchild = curchild->next;
	    if (curchild->fake_thread)
	    {
	      unlink_message (&tmp->child, curchild);
	      insert_message (&parent->child, parent, curchild);
	    }
	    curchild = nextchild;
	  }
	}

	while (!tmp->next && tmp != cur)
	{
	  tmp = tmp->parent;
	}
	if (tmp == cur)
	  break;
	tmp = tmp->next;
      }
    }
  }
  ctx->tree = top;
}


void mutt_clear_threads (CONTEXT *ctx)
{
  int i;

  for (i = 0; i < ctx->msgcount; i++)
  {
    /* mailbox may have been only partially read */
    if (ctx->hdrs[i])
    {
      ctx->hdrs[i]->thread = NULL;
      ctx->hdrs[i]->threaded = 0;
    }
  }
  ctx->tree = NULL;

  if (ctx->thread_hash)
    hash_destroy (&ctx->thread_hash, *free);
}

static int compare_aux_threads (const void *a, const void *b)
{
  static sort_t *aux_func = NULL;
  int rc;

  if (!(a && b))
  {
    aux_func = mutt_get_sort_func (SortAux);
    return aux_func ? 1 : 0;
  }

  rc = (*aux_func) (&(*((THREAD **) a))->sort_aux_key,
                    &(*((THREAD **) b))->sort_aux_key);
  if (rc)
    return (SortAux & SORT_REVERSE) ? -rc : rc;

  rc = mutt_numeric_cmp ((*((THREAD **)a))->sort_aux_key->index,
                         (*((THREAD **)b))->sort_aux_key->index);
  if (rc)
    return (SortAux & SORT_REVERSE) ? -rc : rc;

  return rc;
}

/* This is used to compare individual HEADERS for assigning to
 * sort_aux_key of a parent.
 *
 * Note: the comparison for updating sortkeys does not take REVERSE into
 * account.
 */
static int compare_aux_sortkeys (const void *a, const void *b)
{
  static sort_t *sort_func = NULL;
  int rc;

  if (!(a && b))
  {
    sort_func = mutt_get_sort_func (SortAux);
    return sort_func ? 1 : 0;
  }

  rc = (*sort_func) (a, b);
  if (rc)
    return rc;

  return mutt_numeric_cmp ((*((HEADER **)a))->index, (*((HEADER **)b))->index);
}

static int compare_root_threads (const void *a, const void *b)
{
  static sort_t *sort_func = NULL;
  static int reverse = 0;
  int rc;

  if (!(a && b))
  {
    /* Delegate to the $sort_aux function in this case. */
    if ((SortThreadGroups & SORT_MASK) == SORT_AUX)
    {
      sort_func = mutt_get_sort_func (SortAux);
      reverse = SortAux & SORT_REVERSE;
    }
    else
    {
      sort_func = mutt_get_sort_func (SortThreadGroups);
      reverse = SortThreadGroups & SORT_REVERSE;
    }
    return sort_func ? 1 : 0;
  }

  rc = (*sort_func) (&(*((THREAD **) a))->sort_group_key,
                     &(*((THREAD **) b))->sort_group_key);
  if (rc)
    return reverse ? -rc : rc;

  rc = mutt_numeric_cmp ((*((THREAD **)a))->sort_group_key->index,
                         (*((THREAD **)b))->sort_group_key->index);
  if (rc)
    return reverse ? -rc : rc;

  return rc;
}

/* This is used to compare individual HEADERS for assigning to
 * sort_group_key of a parent.
 *
 * It isn't used when $sort_thread_groups is SORT_AUX, so we don't
 * set up delegation for that case.
 *
 * Note: the comparison for updating sortkeys does not take REVERSE
 * into account.
 */
static int compare_group_sortkeys (const void *a, const void *b)
{
  static sort_t *sort_func = NULL;
  int rc;

  if (!(a && b))
  {
    if ((SortThreadGroups & SORT_MASK) == SORT_AUX)
      return 1;
    sort_func = mutt_get_sort_func (SortThreadGroups);
    return sort_func ? 1 : 0;
  }

  rc = (*sort_func) (a, b);
  if (rc)
    return rc;

  return mutt_numeric_cmp ((*((HEADER **)a))->index, (*((HEADER **)b))->index);
}

THREAD *mutt_sort_subthreads (THREAD *thread, int init)
{
  THREAD **array, *top, *last_child;
  HEADER *new_sort_aux_key, *old_sort_aux_key;
  HEADER *old_sort_group_key;
  int i, array_size, sort_top = 0;

  /* we put things into the array backwards to save some cycles,
   * but we want to have to move less stuff around if we're
   * resorting, so we sort backwards and then put them back
   * in reverse order so they're forwards
   */
  SortAux ^= SORT_REVERSE;
  SortThreadGroups ^= SORT_REVERSE;

  /* Init the comparison functions.  This is done after the above
   * REVERSE flipping because some of these cache sort values. */
  if (!compare_aux_threads (NULL, NULL) ||
      !compare_aux_sortkeys (NULL, NULL) ||
      !compare_root_threads (NULL, NULL) ||
      !compare_group_sortkeys (NULL, NULL))
  {
    SortAux ^= SORT_REVERSE;
    SortThreadGroups ^= SORT_REVERSE;
    return (thread);
  }

  top = thread;

  array = safe_calloc ((array_size = 256), sizeof (THREAD *));
  while (1)
  {
    if (init)
    {
      thread->sort_aux_key = NULL;
      thread->sort_group_key = NULL;
    }
    if (!thread->sort_aux_key && thread->parent)
    {
      thread->parent->recalc_aux_key = 1;
      thread->parent->sort_children = 1;
    }
    if (!thread->sort_group_key)
    {
      if (thread->parent)
        thread->parent->recalc_group_key = 1;
      else
        sort_top = 1;
    }

    if (thread->child)
    {
      thread = thread->child;
      continue;
    }
    else
    {
      /* if it has no children, it must be real. sort it on its own merits */
      thread->sort_aux_key = thread->message;
      thread->sort_group_key = thread->message;

      if (thread->next)
      {
	thread = thread->next;
	continue;
      }
    }

    while (!thread->next)
    {
      /* if it has siblings and needs to be sorted, sort it... */
      if (thread->prev && (thread->parent ? thread->parent->sort_children : sort_top))
      {
        int has_parent = (thread->parent != NULL);
	/* put them into the array */
	for (i = 0; thread; i++, thread = thread->prev)
	{
	  if (i >= array_size)
	    safe_realloc (&array, (array_size *= 2) * sizeof (THREAD *));

	  array[i] = thread;
	}

	qsort ((void *) array, i, sizeof (THREAD *),
               has_parent ? compare_aux_threads : compare_root_threads);

	/* attach them back together.  make thread the last sibling. */
	thread = array[0];
	thread->next = NULL;
	array[i - 1]->prev = NULL;

	if (thread->parent)
	  thread->parent->child = array[i - 1];
	else
	  top = array[i - 1];

	while (--i)
	{
	  array[i - 1]->prev = array[i];
	  array[i]->next = array[i - 1];
	}

        if (thread->parent)
          thread->parent->recalc_aux_key = 1;
      }

      if (thread->parent)
      {
	last_child = thread;
	thread = thread->parent;
        /* we just sorted its children */
        thread->sort_children = 0;

	if (!thread->sort_aux_key || thread->recalc_aux_key)
	{
          thread->recalc_aux_key = 0;
	  old_sort_aux_key = thread->sort_aux_key;
	  thread->sort_aux_key = thread->message;

	  /* make sort_aux_key the first or last sibling, as appropriate.
           * note that SortAux's SORT_REVERSE is flipped:
           *   - if SORT_LAST, new_sort_aux_key will be the greatest value.
           *   - otherwise, new_sort_aux_key will be the least value. */
	  new_sort_aux_key = (!(SortAux & SORT_LAST) ^ !(SortAux & SORT_REVERSE)) ?
            thread->child->sort_aux_key : last_child->sort_aux_key;

	  if (!thread->sort_aux_key)
	    thread->sort_aux_key = new_sort_aux_key;
	  else if (SortAux & SORT_LAST)
	  {
	    if (compare_aux_sortkeys (&thread->sort_aux_key,
                                      &new_sort_aux_key) < 0)
	      thread->sort_aux_key = new_sort_aux_key;
	  }

	  if ((old_sort_aux_key != thread->sort_aux_key) && thread->parent)
	  {
            thread->parent->recalc_aux_key = 1;
            thread->parent->sort_children = 1;
	  }
        }

	if (!thread->sort_group_key || thread->recalc_group_key)
	{
          thread->recalc_group_key = 0;
	  old_sort_group_key = thread->sort_group_key;

          /* If $sort_thread_groups is turned off, just use the result
           * of $sort_aux.  If it's the same as $sort_aux, do the
           * same.
           *
           * NOTE: SORT_REVERSE is ignored here because it just
           * determines the order of the children, not the selection
           * of least/greatest.
           */
          if (((SortThreadGroups & SORT_MASK) == SORT_AUX) ||
              ((SortThreadGroups & ~SORT_REVERSE) == (SortAux & ~SORT_REVERSE)))
            thread->sort_group_key = thread->sort_aux_key;
          else
          {
            thread->sort_group_key = thread->message;
            if (!thread->sort_group_key)
            {
              thread->sort_group_key = last_child->sort_group_key;

              /* If SORT_LAST is unset, fill the placeholder thread key
               * with the least value in the children, just like with $sort_aux.
               */
              if (!(SortThreadGroups & SORT_LAST))
              {
                for (THREAD *tmp = last_child->prev; tmp; tmp = tmp->prev)
                {
                  if (compare_group_sortkeys (&thread->sort_group_key,
                                             &tmp->sort_group_key) > 0)
                    thread->sort_group_key = tmp->sort_group_key;
                }
              }
            }

            /* Scan for the greatest value in the children */
            if (SortThreadGroups & SORT_LAST)
            {
              for (THREAD *tmp = last_child; tmp; tmp = tmp->prev)
              {
                if (compare_group_sortkeys (&thread->sort_group_key,
                                           &tmp->sort_group_key) < 0)
                  thread->sort_group_key = tmp->sort_group_key;
              }
            }
          }

	  if (old_sort_group_key != thread->sort_group_key)
	  {
	    if (thread->parent)
              thread->parent->recalc_group_key = 1;
            else
	      sort_top = 1;
	  }
	}
      }
      else
      {
	SortAux ^= SORT_REVERSE;
	SortThreadGroups ^= SORT_REVERSE;
	FREE (&array);
	return (top);
      }
    }

    thread = thread->next;
  }
}

static void check_subjects (CONTEXT *ctx, int init)
{
  HEADER *cur;
  THREAD *tmp;
  int i;

  for (i = 0; i < ctx->msgcount; i++)
  {
    cur = ctx->hdrs[i];
    if (cur->thread->check_subject)
      cur->thread->check_subject = 0;
    else if (!init)
      continue;

    /* figure out which messages have subjects different than their parents' */
    tmp = cur->thread->parent;
    while (tmp && !tmp->message)
    {
      tmp = tmp->parent;
    }

    if (!tmp)
      cur->subject_changed = 1;
    else if (cur->env->real_subj && tmp->message->env->real_subj)
      cur->subject_changed = mutt_strcmp (cur->env->real_subj,
					  tmp->message->env->real_subj) ? 1 : 0;
    else
      cur->subject_changed = (cur->env->real_subj
			      || tmp->message->env->real_subj) ? 1 : 0;
  }
}

void mutt_sort_threads (CONTEXT *ctx, int init)
{
  HEADER *cur;
  int i, using_refs = 0;
  THREAD *thread, *new, *tmp, top;
  LIST *ref = NULL;

  if (!ctx->thread_hash)
    init = 1;

  if (init)
    ctx->thread_hash = hash_create (ctx->msgcount * 2, MUTT_HASH_ALLOW_DUPS);

  /* we want a quick way to see if things are actually attached to the top of the
   * thread tree or if they're just dangling, so we attach everything to a top
   * node temporarily */
  top.parent = top.next = top.prev = NULL;
  top.child = ctx->tree;
  for (thread = ctx->tree; thread; thread = thread->next)
    thread->parent = &top;

  /* put each new message together with the matching messageless THREAD if it
   * exists.  otherwise, if there is a THREAD that already has a message, thread
   * new message as an identical child.  if we didn't attach the message to a
   * THREAD, make a new one for it. */
  for (i = 0; i < ctx->msgcount; i++)
  {
    cur = ctx->hdrs[i];

    if (!cur->thread)
    {
      if ((!init || option (OPTDUPTHREADS)) && cur->env->message_id)
	thread = hash_find (ctx->thread_hash, cur->env->message_id);
      else
	thread = NULL;

      if (thread && !thread->message)
      {
	/* this is a message which was missing before */
	thread->message = cur;
	cur->thread = thread;
	thread->check_subject = 1;

	/* mark descendants as needing subject_changed checked */
	for (tmp = (thread->child ? thread->child : thread); tmp != thread; )
	{
	  while (!tmp->message)
	    tmp = tmp->child;
	  tmp->check_subject = 1;
	  while (!tmp->next && tmp != thread)
	    tmp = tmp->parent;
	  if (tmp != thread)
	    tmp = tmp->next;
	}

	if (thread->parent)
	{
	  /* remove threading info above it based on its children, which we'll
	   * recalculate based on its headers.  make sure not to leave
	   * dangling missing messages.  note that we haven't kept track
	   * of what info came from its children and what from its siblings'
	   * children, so we just remove the stuff that's definitely from it */
	  do
	  {
	    tmp = thread->parent;
	    unlink_message (&tmp->child, thread);
	    thread->parent = NULL;
	    thread->sort_aux_key = NULL;
	    thread->sort_group_key = NULL;
	    thread->fake_thread = 0;
	    thread = tmp;
	  } while (thread != &top && !thread->child && !thread->message);
	}
      }
      else
      {
	new = (option (OPTDUPTHREADS) ? thread : NULL);

	thread = safe_calloc (1, sizeof (THREAD));
	thread->message = cur;
	thread->check_subject = 1;
	cur->thread = thread;
	hash_insert (ctx->thread_hash,
		     cur->env->message_id ? cur->env->message_id : "",
		     thread);

	if (new)
	{
	  if (new->duplicate_thread)
	    new = new->parent;

	  thread = cur->thread;

	  insert_message (&new->child, new, thread);
	  thread->duplicate_thread = 1;
	  thread->message->threaded = 1;
	}
      }
    }
    else
    {
      /* unlink pseudo-threads because they might be children of newly
       * arrived messages */
      thread = cur->thread;
      for (new = thread->child; new; )
      {
	tmp = new->next;
	if (new->fake_thread)
	{
	  unlink_message (&thread->child, new);
	  insert_message (&top.child, &top, new);
	  new->fake_thread = 0;
	}
	new = tmp;
      }
    }
  }

  /* thread by references */
  for (i = 0; i < ctx->msgcount; i++)
  {
    cur = ctx->hdrs[i];
    if (cur->threaded)
      continue;
    cur->threaded = 1;

    thread = cur->thread;
    using_refs = 0;

    while (1)
    {
      if (using_refs == 0)
      {
	/* look at the beginning of in-reply-to: */
	if ((ref = cur->env->in_reply_to) != NULL)
	  using_refs = 1;
	else
	{
	  ref = cur->env->references;
	  using_refs = 2;
	}
      }
      else if (using_refs == 1)
      {
	/* if there's no references header, use all the in-reply-to:
	 * data that we have.  otherwise, use the first reference
	 * if it's different than the first in-reply-to, otherwise use
	 * the second reference (since at least eudora puts the most
	 * recent reference in in-reply-to and the rest in references)
	 */
	if (!cur->env->references)
	  ref = ref->next;
	else
	{
	  if (mutt_strcmp (ref->data, cur->env->references->data))
	    ref = cur->env->references;
	  else
	    ref = cur->env->references->next;

	  using_refs = 2;
	}
      }
      else
	ref = ref->next; /* go on with references */

      if (!ref)
	break;

      if ((new = hash_find (ctx->thread_hash, ref->data)) == NULL)
      {
	new = safe_calloc (1, sizeof (THREAD));
	hash_insert (ctx->thread_hash, ref->data, new);
      }
      else
      {
	if (new->duplicate_thread)
	  new = new->parent;
	if (is_descendant (new, thread)) /* no loops! */
	  continue;
      }

      if (thread->parent)
	unlink_message (&top.child, thread);
      insert_message (&new->child, new, thread);
      thread = new;
      if (thread->message || (thread->parent && thread->parent != &top))
	break;
    }

    if (!thread->parent)
      insert_message (&top.child, &top, thread);
  }

  /* detach everything from the temporary top node */
  for (thread = top.child; thread; thread = thread->next)
  {
    thread->parent = NULL;
  }
  ctx->tree = top.child;

  check_subjects (ctx, init);

  if (!option (OPTSTRICTTHREADS))
    pseudo_threads (ctx);

  if (ctx->tree)
  {
    ctx->tree = mutt_sort_subthreads (ctx->tree, init);

    /* Put the list into an array. */
    linearize_tree (ctx);

    /* Draw the thread tree. */
    mutt_draw_tree (ctx);
  }
}

static HEADER *find_virtual (THREAD *cur, int reverse)
{
  THREAD *top;

  if (cur->message && cur->message->virtual >= 0)
    return (cur->message);

  top = cur;
  if ((cur = cur->child) == NULL)
    return (NULL);

  while (reverse && cur->next)
    cur = cur->next;

  FOREVER
  {
    if (cur->message && cur->message->virtual >= 0)
      return (cur->message);

    if (cur->child)
    {
      cur = cur->child;

      while (reverse && cur->next)
	cur = cur->next;
    }
    else if (reverse ? cur->prev : cur->next)
      cur = reverse ? cur->prev : cur->next;
    else
    {
      while (!(reverse ? cur->prev : cur->next))
      {
	cur = cur->parent;
	if (cur == top)
	  return (NULL);
      }
      cur = reverse ? cur->prev : cur->next;
    }
    /* not reached */
  }
}

/* dir => true when moving forward, false when moving in reverse
 * subthreads => false when moving to next thread, true when moving to next subthread
 */
int _mutt_aside_thread (HEADER *hdr, short dir, short subthreads)
{
  THREAD *cur;
  HEADER *tmp;

  if ((Sort & SORT_MASK) != SORT_THREADS)
  {
    mutt_error _("Threading is not enabled.");
    return (hdr->virtual);
  }

  cur = hdr->thread;

  if (!subthreads)
  {
    while (cur->parent)
      cur = cur->parent;
  }
  else
  {
    if ((dir != 0) ^ ((Sort & SORT_REVERSE) != 0))
    {
      while (!cur->next && cur->parent)
	cur = cur->parent;
    }
    else
    {
      while (!cur->prev && cur->parent)
	cur = cur->parent;
    }
  }

  if ((dir != 0) ^ ((Sort & SORT_REVERSE) != 0))
  {
    do
    {
      cur = cur->next;
      if (!cur)
	return (-1);
      tmp = find_virtual (cur, 0);
    } while (!tmp);
  }
  else
  {
    do
    {
      cur = cur->prev;
      if (!cur)
	return (-1);
      tmp = find_virtual (cur, 1);
    } while (!tmp);
  }

  return (tmp->virtual);
}

int mutt_parent_message (CONTEXT *ctx, HEADER *hdr, int find_root)
{
  THREAD *thread;
  HEADER *parent = NULL;

  if ((Sort & SORT_MASK) != SORT_THREADS)
  {
    mutt_error _("Threading is not enabled.");
    return (hdr->virtual);
  }

  /* Root may be the current message */
  if (find_root)
    parent = hdr;

  for (thread = hdr->thread->parent; thread; thread = thread->parent)
  {
    if ((hdr = thread->message) != NULL)
    {
      parent = hdr;
      if (!find_root)
        break;
    }
  }

  if (!parent)
  {
    mutt_error _("Parent message is not available.");
    return (-1);
  }
  if (!VISIBLE (parent, ctx))
  {
    if (find_root)
      mutt_error _("Root message is not visible in this limited view.");
    else
      mutt_error _("Parent message is not visible in this limited view.");
    return (-1);
  }
  return (parent->virtual);
}

void mutt_set_virtual (CONTEXT *ctx)
{
  int i, padding;
  HEADER *cur;

  ctx->vcount = 0;
  ctx->vsize = 0;
  padding = mx_msg_padding_size (ctx);

  for (i = 0; i < ctx->msgcount; i++)
  {
    cur = ctx->hdrs[i];
    if (cur->virtual >= 0)
    {
      cur->virtual = ctx->vcount;
      ctx->v2r[ctx->vcount] = i;
      ctx->vcount++;
      ctx->vsize += cur->content->length + cur->content->offset -
        cur->content->hdr_offset + padding;
    }
  }
}

int _mutt_traverse_thread (CONTEXT *ctx, HEADER *cur, int flag)
{
  THREAD *thread, *top;
  HEADER *roothdr = NULL;
  int final;
  int num_hidden = 0, new = 0, old = 0;
  int min_unread_msgno = INT_MAX, min_unread = cur->virtual;
#define CHECK_LIMIT (!ctx->pattern || cur->limited)

  if ((Sort & SORT_MASK) != SORT_THREADS)
  {
    mutt_error (_("Threading is not enabled."));
    return (cur->virtual);
  }

  final = cur->virtual;
  thread = cur->thread;
  while (thread->parent)
    thread = thread->parent;
  top = thread;
  while (!thread->message)
    thread = thread->child;
  cur = thread->message;

  if (!cur->read && CHECK_LIMIT)
  {
    if (cur->old)
      old = 2;
    else
      new = 1;
    if (cur->msgno < min_unread_msgno)
    {
      min_unread = cur->virtual;
      min_unread_msgno = cur->msgno;
    }
  }

  if (cur->virtual == -1 && CHECK_LIMIT)
    num_hidden++;

  if (flag & (MUTT_THREAD_COLLAPSE | MUTT_THREAD_UNCOLLAPSE))
  {
    cur->color.pair = cur->color.attrs = 0; /* force index entry's color to be re-evaluated */
    cur->collapsed = flag & MUTT_THREAD_COLLAPSE;
    if (cur->virtual != -1)
    {
      roothdr = cur;
      if (flag & MUTT_THREAD_COLLAPSE)
	final = roothdr->virtual;
    }
  }

  if (thread == top && (thread = thread->child) == NULL)
  {
    /* return value depends on action requested */
    if (flag & (MUTT_THREAD_COLLAPSE | MUTT_THREAD_UNCOLLAPSE))
    {
      cur->num_hidden = num_hidden;
      return (final);
    }
    else if (flag & MUTT_THREAD_UNREAD)
      return ((old && new) ? new : (old ? old : new));
    else if (flag & MUTT_THREAD_NEXT_UNREAD)
      return (min_unread);
  }

  FOREVER
  {
    cur = thread->message;

    if (cur)
    {
      if (flag & (MUTT_THREAD_COLLAPSE | MUTT_THREAD_UNCOLLAPSE))
      {
	cur->color.pair = cur->color.attrs = 0; /* force index entry's color to be re-evaluated */
	cur->collapsed = flag & MUTT_THREAD_COLLAPSE;
	if (!roothdr && CHECK_LIMIT)
	{
	  roothdr = cur;
	  if (flag & MUTT_THREAD_COLLAPSE)
	    final = roothdr->virtual;
	}

	if (flag & MUTT_THREAD_COLLAPSE)
	{
	  if (cur != roothdr)
	    cur->virtual = -1;
	}
	else
	{
	  if (CHECK_LIMIT)
	    cur->virtual = cur->msgno;
	}
      }


      if (!cur->read && CHECK_LIMIT)
      {
	if (cur->old)
	  old = 2;
	else
	  new = 1;
	if (cur->msgno < min_unread_msgno)
	{
	  min_unread = cur->virtual;
	  min_unread_msgno = cur->msgno;
	}
      }

      if (cur->virtual == -1 && CHECK_LIMIT)
	num_hidden++;
    }

    if (thread->child)
      thread = thread->child;
    else if (thread->next)
      thread = thread->next;
    else
    {
      int done = 0;
      while (!thread->next)
      {
	thread = thread->parent;
	if (thread == top)
	{
	  done = 1;
	  break;
	}
      }
      if (done)
	break;
      thread = thread->next;
    }
  }

  /* retraverse the thread and store num_hidden in all headers, with
   * or without a virtual index.  this will allow ~v to match all
   * collapsed messages when switching sort order to non-threaded.
   */
  if (flag & MUTT_THREAD_COLLAPSE)
  {
    thread = top;
    FOREVER
    {
      cur = thread->message;
      if (cur)
        cur->num_hidden = num_hidden + 1;

      if (thread->child)
        thread = thread->child;
      else if (thread->next)
        thread = thread->next;
      else
      {
        int done = 0;
        while (!thread->next)
        {
          thread = thread->parent;
          if (thread == top)
          {
            done = 1;
            break;
          }
        }
        if (done)
          break;
        thread = thread->next;
      }
    }
  }

  /* return value depends on action requested */
  if (flag & (MUTT_THREAD_COLLAPSE | MUTT_THREAD_UNCOLLAPSE))
    return (final);
  else if (flag & MUTT_THREAD_UNREAD)
    return ((old && new) ? new : (old ? old : new));
  else if (flag & MUTT_THREAD_NEXT_UNREAD)
    return (min_unread);

  return (0);
#undef CHECK_LIMIT
}


/* if flag is 0, we want to know how many messages
 * are in the thread.  if flag is 1, we want to know
 * our position in the thread. */
int mutt_messages_in_thread (CONTEXT *ctx, HEADER *hdr, int flag)
{
  THREAD *threads[2];
  int i, rc;

  if ((Sort & SORT_MASK) != SORT_THREADS || !hdr->thread)
    return (1);

  threads[0] = hdr->thread;
  while (threads[0]->parent)
    threads[0] = threads[0]->parent;

  threads[1] = flag ? hdr->thread : threads[0]->next;

  for (i = 0; i < ((flag || !threads[1]) ? 1 : 2); i++)
  {
    while (!threads[i]->message)
      threads[i] = threads[i]->child;
  }

  if (Sort & SORT_REVERSE)
    rc = threads[0]->message->msgno - (threads[1] ? threads[1]->message->msgno : -1);
  else
    rc = (threads[1] ? threads[1]->message->msgno : ctx->msgcount) - threads[0]->message->msgno;

  if (flag)
    rc += 1;

  return (rc);
}


HASH *mutt_make_id_hash (CONTEXT *ctx)
{
  int i;
  HEADER *hdr;
  HASH *hash;

  hash = hash_create (ctx->msgcount * 2, 0);

  for (i = 0; i < ctx->msgcount; i++)
  {
    hdr = ctx->hdrs[i];
    if (hdr->env->message_id)
      hash_insert (hash, hdr->env->message_id, hdr);
  }

  return hash;
}

HASH *mutt_make_subj_hash (CONTEXT *ctx)
{
  int i;
  HEADER *hdr;
  HASH *hash;

  hash = hash_create (ctx->msgcount * 2, MUTT_HASH_ALLOW_DUPS);

  for (i = 0; i < ctx->msgcount; i++)
  {
    hdr = ctx->hdrs[i];
    if (hdr->env->real_subj)
      hash_insert (hash, hdr->env->real_subj, hdr);
  }

  return hash;
}

static void clean_references (THREAD *brk, THREAD *cur)
{
  THREAD *p;
  LIST *ref = NULL;
  int done = 0;

  for (; cur; cur = cur->next, done = 0)
  {
    /* parse subthread recursively */
    clean_references (brk, cur->child);

    if (!cur->message)
      break; /* skip pseudo-message */

    /* Looking for the first bad reference according to the new threading.
     * Optimal since Mutt stores the references in reverse order, and the
     * first loop should match immediately for mails respecting RFC2822. */
    for (p = brk; !done && p; p = p->parent)
      for (ref = cur->message->env->references; p->message && ref; ref = ref->next)
	if (!mutt_strcasecmp (ref->data, p->message->env->message_id))
	{
	  done = 1;
	  break;
	}

    if (done)
    {
      HEADER *h = cur->message;

      /* clearing the References: header from obsolete Message-ID(s) */
      mutt_free_list (&ref->next);

      h->changed = 1;
      h->env->changed |= MUTT_ENV_CHANGED_REFS;
    }
  }
}

void mutt_break_thread (HEADER *hdr)
{
  mutt_free_list (&hdr->env->in_reply_to);
  mutt_free_list (&hdr->env->references);
  hdr->changed = 1;
  hdr->env->changed |= (MUTT_ENV_CHANGED_IRT | MUTT_ENV_CHANGED_REFS);

  clean_references (hdr->thread, hdr->thread->child);
}

static int link_threads (HEADER *parent, HEADER *child, CONTEXT *ctx)
{
  if (child == parent)
    return 0;

  mutt_break_thread (child);

  child->env->in_reply_to = mutt_new_list ();
  child->env->in_reply_to->data = safe_strdup (parent->env->message_id);

  mutt_set_flag (ctx, child, MUTT_TAG, 0);

  child->changed = 1;
  child->env->changed |= MUTT_ENV_CHANGED_IRT;
  return 1;
}

int mutt_link_threads (HEADER *cur, HEADER *last, CONTEXT *ctx)
{
  int i, changed = 0;

  if (!last)
  {
    for (i = 0; i < ctx->vcount; i++)
      if (ctx->hdrs[Context->v2r[i]]->tagged)
	changed |= link_threads (cur, ctx->hdrs[Context->v2r[i]], ctx);
  }
  else
    changed = link_threads (cur, last, ctx);

  return changed;
}
