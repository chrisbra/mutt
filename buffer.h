/*
 * Copyright (C) 1996-2002,2010,2013 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2004 g10 Code GmbH
 * Copyright (C) 2018 Kevin J. McCarthy <kevin@8t8.us>
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

#ifndef _BUFFER_H
#define _BUFFER_H

typedef struct
{
  char *data;	/* pointer to data */
  char *dptr;	/* current read/write position */
  size_t dsize;	/* length of data */
  int destroy;	/* destroy `data' when done? */
} BUFFER;

BUFFER *mutt_buffer_new (void);
BUFFER * mutt_buffer_init (BUFFER *);
void mutt_buffer_increase_size (BUFFER *, size_t);
BUFFER * mutt_buffer_from (char *);
void mutt_buffer_free(BUFFER **);
int mutt_buffer_printf (BUFFER*, const char*, ...);
void mutt_buffer_add (BUFFER*, const char*, size_t);
void mutt_buffer_addstr (BUFFER*, const char*);
void mutt_buffer_addch (BUFFER*, char);

#endif