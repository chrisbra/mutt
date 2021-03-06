/*
 * Copyright (C) 2002     Manuel Novoa III
 * Copyright (C) 2000-2005 Erik Andersen <andersen@uclibc.org>
 *
 * Licensed under the LGPL v2.1, see the file COPYING.LIB in this tarball.
 */
#include <stdlib.h>
#include <ctype.h>

char *strcasestr(const char *s1, const char *s2)
{
        register const char *s = s1;
        register const char *p = s2;

        do {
                if (!*p) {
                        return (char *) s1;;
                }
                if ((*p == *s)
                        || (tolower(*((unsigned char *)p)) == tolower(*((unsigned char *)s)))
                        ) {
                        ++p;
                        ++s;
                } else {
                        p = s2;
                        if (!*s) {
                                return NULL;
                        }
                        s = ++s1;
                }
        } while (1);
}
