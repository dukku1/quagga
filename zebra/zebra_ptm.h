/*
 * Definitions for prescriptive topology module (PTM).
 * Copyright (C) 1998, 99, 2000 Kunihiro Ishiguro, Toshiaki Takada
 *
 * This file is part of GNU Zebra.
 *
 * GNU Zebra is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2, or (at your option) any
 * later version.
 *
 * GNU Zebra is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with GNU Zebra; see the file COPYING.  If not, write to the Free
 * Software Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */

#ifndef _ZEBRA_PTM_H
#define _ZEBRA_PTM_H

extern const char ZEBRA_PTM_SOCK_NAME[];
#define ZEBRA_PTM_MAX_SOCKBUF 3200 /* 25B *128 ports */
#define ZEBRA_PTM_SEND_MAX_SOCKBUF 512
extern int ptm_enable;

void zebra_ptm_init (void);
int zebra_ptm_connect (struct thread *t);
void zebra_ptm_write (struct vty *vty);

int zebra_ptm_bfd_dst_register (struct zserv *client, int sock, u_short length,
                                  int command);
int zebra_ptm_bfd_dst_deregister (struct zserv *client, int sock, u_short length);
#endif
