/*
 * Copyright (C) 2021 Kevin J. McCarthy <kevin@8t8.us>
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

#ifndef _MUTT_SASL_GNU_H_
#define _MUTT_SASL_GNU_H_ 1

#include <gsasl.h>

#include "mutt_socket.h"

void mutt_gsasl_done (void);
const char *mutt_gsasl_get_mech (const char *requested_mech,
                                 const char *server_mechlist);
int mutt_gsasl_client_new (CONNECTION *, const char *, Gsasl_session **);
void mutt_gsasl_client_finish (Gsasl_session **sctx);

#endif /* _MUTT_SASL_GNU_H_ */
