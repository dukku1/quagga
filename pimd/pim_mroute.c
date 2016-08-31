/*
  PIM for Quagga
  Copyright (C) 2008  Everton da Silva Marques

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
  General Public License for more details.
  
  You should have received a copy of the GNU General Public License
  along with this program; see the file COPYING; if not, write to the
  Free Software Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA 02110-1301 USA
  
*/

#include <zebra.h>
#include "log.h"
#include "privs.h"
#include "if.h"
#include "prefix.h"

#include "pimd.h"
#include "pim_rpf.h"
#include "pim_mroute.h"
#include "pim_oil.h"
#include "pim_str.h"
#include "pim_time.h"
#include "pim_iface.h"
#include "pim_macro.h"
#include "pim_rp.h"
#include "pim_oil.h"
#include "pim_register.h"
#include "pim_ifchannel.h"
#include "pim_zlookup.h"

/* GLOBAL VARS */
extern struct zebra_privs_t pimd_privs;

static void mroute_read_on(void);

static int pim_mroute_set(int fd, int enable)
{
  int err;
  int opt = enable ? MRT_INIT : MRT_DONE;
  socklen_t opt_len = sizeof(opt);

  err = setsockopt(fd, IPPROTO_IP, opt, &opt, opt_len);
  if (err) {
    zlog_warn("%s %s: failure: setsockopt(fd=%d,IPPROTO_IP,%s=%d): errno=%d: %s",
	      __FILE__, __PRETTY_FUNCTION__,
	      fd, enable ? "MRT_INIT" : "MRT_DONE", opt, errno, safe_strerror(errno));
    return -1;
  }

  if (enable)
    {
      int upcalls = IGMPMSG_WRVIFWHOLE;
      opt = MRT_PIM;
    
      err = setsockopt (fd, IPPROTO_IP, opt, &upcalls, sizeof (upcalls));
      if (err)
        {
          zlog_warn ("Failure to register for VIFWHOLE and WRONGVIF upcalls %d %s",
		     errno, safe_strerror (errno));
          return -1;
        }
    }
  
  return 0;
}

static const char *igmpmsgtype2str[IGMPMSG_WRVIFWHOLE + 1] = {
  "<unknown_upcall?>",
  "NOCACHE",
  "WRONGVIF",
  "WHOLEPKT",
  "WRVIFWHOLE" };

static int
pim_mroute_msg_nocache (int fd, struct interface *ifp, const struct igmpmsg *msg)
{
  struct pim_interface *pim_ifp = ifp->info;
  struct pim_upstream *up;
  struct pim_rpf *rpg;
  struct prefix_sg sg;
  struct channel_oil *oil;

  rpg = RP(msg->im_dst);
  /*
   * If the incoming interface is unknown OR
   * the Interface type is SSM we don't need to
   * do anything here
   */
  if ((rpg->rpf_addr.s_addr == INADDR_NONE) ||
      (!pim_ifp) ||
      (!(PIM_I_am_DR(pim_ifp))) ||
      (pim_ifp->itype == PIM_INTERFACE_SSM))
    return 0;

  /*
   * If we've received a multicast packet that isn't connected to
   * us
   */
  if (!pim_if_connected_to_source (ifp, msg->im_src))
    {
      if (PIM_DEBUG_MROUTE_DETAIL)
       zlog_debug ("%s: Received incoming packet that doesn't originate on our seg",
		   __PRETTY_FUNCTION__);
      return 0;
    }

  memset (&sg, 0, sizeof (struct prefix_sg));
  sg.src = msg->im_src;
  sg.grp = msg->im_dst;

  if (PIM_DEBUG_MROUTE) {
    zlog_debug("%s: Adding a Route %s for WHOLEPKT consumption",
	       __PRETTY_FUNCTION__, pim_str_sg_dump (&sg));
  }

  oil = pim_channel_oil_add (&sg, pim_ifp->mroute_vif_index);
  if (!oil) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: Failure to add channel oil for %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg));
    }
    return 0;
  }

  up = pim_upstream_add (&sg, ifp);
  if (!up) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: Failure to add upstream information for %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg));
    }
    return 0;
  }

  pim_upstream_keep_alive_timer_start (up, qpim_keep_alive_time);

  up->channel_oil = oil;
  up->channel_oil->cc.pktcnt++;
  up->fhr = 1;
  pim_channel_add_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
  up->join_state = PIM_UPSTREAM_JOINED;

  return 0;
}

static int
pim_mroute_msg_wholepkt (int fd, struct interface *ifp, const char *buf)
{
  struct pim_interface *pim_ifp;
  struct prefix_sg sg;
  struct pim_rpf *rpg;
  const struct ip *ip_hdr;
  struct pim_upstream *up;

  ip_hdr = (const struct ip *)buf;

  memset (&sg, 0, sizeof (struct prefix_sg));
  sg.src = ip_hdr->ip_src;
  sg.grp = ip_hdr->ip_dst;

  up = pim_upstream_find(&sg);
  if (!up) {
    if (PIM_DEBUG_MROUTE_DETAIL) {
      zlog_debug("%s: Unable to find upstream channel WHOLEPKT%s",
		 __PRETTY_FUNCTION__, pim_str_sg_dump (&sg));
    }
    return 0;
  }

  pim_ifp = up->rpf.source_nexthop.interface->info;

  rpg = RP(sg.grp);

  if ((rpg->rpf_addr.s_addr == INADDR_NONE) ||
      (!pim_ifp) ||
      (!(PIM_I_am_DR(pim_ifp))) ||
      (pim_ifp->itype == PIM_INTERFACE_SSM)) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: Failed Check send packet", __PRETTY_FUNCTION__);
    }
    return 0;
  }

  /*
   * If we've received a register suppress
   */
  if (!up->t_rs_timer)
    pim_register_send((uint8_t *)buf + sizeof(struct ip), ntohs (ip_hdr->ip_len),
		      pim_ifp->primary_address, rpg, 0);

  return 0;
}

static int
pim_mroute_msg_wrongvif (int fd, struct interface *ifp, const struct igmpmsg *msg)
{
  struct pim_ifchannel *ch;
  struct pim_interface *pim_ifp;
  struct prefix_sg sg;

  memset (&sg, 0, sizeof (struct prefix_sg));
  sg.src = msg->im_src;
  sg.grp = msg->im_dst;

  /*
    Send Assert(S,G) on iif as response to WRONGVIF kernel upcall.

    RFC 4601 4.8.2.  PIM-SSM-Only Routers

    iif is the incoming interface of the packet.
    if (iif is in inherited_olist(S,G)) {
    send Assert(S,G) on iif
    }
  */

  if (!ifp) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s could not find input interface for input_vif_index=%d",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), msg->im_vif);
    }
    return -1;
  }

  pim_ifp = ifp->info;
  if (!pim_ifp) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s multicast not enabled on interface %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), ifp->name);
    }
    return -2;
  }

  ch = pim_ifchannel_find(ifp, &sg);
  if (!ch) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s could not find channel on interface %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), ifp->name);
    }
    return -3;
  }

  /*
    RFC 4601: 4.6.1.  (S,G) Assert Message State Machine

    Transitions from NoInfo State

    An (S,G) data packet arrives on interface I, AND
    CouldAssert(S,G,I)==TRUE An (S,G) data packet arrived on an
    downstream interface that is in our (S,G) outgoing interface
    list.  We optimistically assume that we will be the assert
    winner for this (S,G), and so we transition to the "I am Assert
    Winner" state and perform Actions A1 (below), which will
    initiate the assert negotiation for (S,G).
  */

  if (ch->ifassert_state != PIM_IFASSERT_NOINFO) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s channel is not on Assert NoInfo state for interface %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), ifp->name);
    }
    return -4;
  }

  if (!PIM_IF_FLAG_TEST_COULD_ASSERT(ch->flags)) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s interface %s is not downstream for channel",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), ifp->name);
    }
    return -5;
  }

  if (assert_action_a1(ch)) {
    if (PIM_DEBUG_MROUTE) {
      zlog_debug("%s: WRONGVIF (S,G)=%s assert_action_a1 failure on interface %s",
		 __PRETTY_FUNCTION__,
		 pim_str_sg_dump (&sg), ifp->name);
    }
    return -6;
  }

  return 0;
}

static int
pim_mroute_msg_wrvifwhole (int fd, struct interface *ifp, const char *buf)
{
  const struct ip *ip_hdr = (const struct ip *)buf;
  struct pim_interface *pim_ifp;
  struct pim_ifchannel *ch;
  struct pim_upstream *up;
  struct prefix_sg sg;
  struct channel_oil *oil;

  memset (&sg, 0, sizeof (struct prefix_sg));
  sg.src = ip_hdr->ip_src;
  sg.grp = ip_hdr->ip_dst;

  if (PIM_DEBUG_MROUTE)
    zlog_debug ("Received WHOLEPKT Wrong Vif for %s on %s",
		pim_str_sg_dump (&sg), ifp->name);

  ch = pim_ifchannel_find(ifp, &sg);
  if (ch)
    {
      if (PIM_DEBUG_MROUTE)
	zlog_debug ("WRVIFWHOLE (S,G)=%s found ifchannel on interface %s",
		    pim_str_sg_dump (&sg), ifp->name);
      return -1;
    }

  if (PIM_DEBUG_MROUTE)
    zlog_debug ("If channel: %p", ch);

  up = pim_upstream_find (&sg);
  if (up)
    {
      /*
       * If we are the fhr that means we are getting a callback during
       * the pimreg period, so I believe we can ignore this packet
       */
      if (!up->fhr)
	{
	  struct pim_nexthop source;
	  struct pim_rpf *rpf = RP (sg.grp);
	  pim_ifp = rpf->source_nexthop.interface->info;

	  //No if channel, but upstream we are at the RP.
	  pim_nexthop_lookup (&source, up->upstream_register);
	  pim_register_stop_send(source.interface, &sg, pim_ifp->primary_address, up->upstream_register);
          if (!up->channel_oil)
            up->channel_oil = pim_channel_oil_add (&sg, pim_ifp->mroute_vif_index);
          if (!up->channel_oil->installed)
            pim_mroute_add (up->channel_oil);
	  //Send S bit down the join.
	  up->sptbit = PIM_UPSTREAM_SPTBIT_TRUE;
	}
	  return 0;
    }

  pim_ifp = ifp->info;
  oil = pim_channel_oil_add (&sg, pim_ifp->mroute_vif_index);
  if (!oil->installed)
    pim_mroute_add (oil);
  if (pim_if_connected_to_source (ifp, sg.src))
    {
      up = pim_upstream_add (&sg, ifp);

      if (!up)
	{
	  if (PIM_DEBUG_MROUTE)
	    zlog_debug ("%s: WRONGVIF%s unable to create upstream on interface",
			pim_str_sg_dump (&sg), ifp->name);
	  return -2;
	}
      up->fhr = 1;

      pim_upstream_keep_alive_timer_start (up, qpim_keep_alive_time);
      up->channel_oil = oil;
      up->channel_oil->cc.pktcnt++;
      pim_channel_add_oif (up->channel_oil, pim_regiface, PIM_OIF_FLAG_PROTO_PIM);
      up->join_state = PIM_UPSTREAM_JOINED;
      pim_upstream_inherited_olist (up);

      // Send the packet to the RP
      pim_mroute_msg_wholepkt (fd, ifp, buf);
    }

  return 0;
}

int pim_mroute_msg(int fd, const char *buf, int buf_size)
{
  struct interface     *ifp;
  const struct ip      *ip_hdr;
  const struct igmpmsg *msg;
  char src_str[100] = "<src?>";
  char grp_str[100] = "<grp?>";

  ip_hdr = (const struct ip *) buf;

  /* kernel upcall must have protocol=0 */
  if (ip_hdr->ip_p) {
    /* this is not a kernel upcall */
    if (PIM_DEBUG_MROUTE_DETAIL) {
      pim_inet4_dump("<src?>", ip_hdr->ip_src, src_str, sizeof(src_str));
      pim_inet4_dump("<grp?>", ip_hdr->ip_dst, grp_str, sizeof(grp_str));
      zlog_debug("%s: not a kernel upcall proto=%d src: %s dst: %s msg_size=%d",
		 __PRETTY_FUNCTION__, ip_hdr->ip_p, src_str, grp_str, buf_size);
    }
    return 0;
  }

  msg = (const struct igmpmsg *) buf;

  ifp = pim_if_find_by_vif_index(msg->im_vif);

  if (PIM_DEBUG_MROUTE) {
    pim_inet4_dump("<src?>", msg->im_src, src_str, sizeof(src_str));
    pim_inet4_dump("<grp?>", msg->im_dst, grp_str, sizeof(grp_str));
    zlog_warn("%s: kernel upcall %s type=%d ip_p=%d from fd=%d for (S,G)=(%s,%s) on %s vifi=%d",
	      __PRETTY_FUNCTION__,
	      igmpmsgtype2str[msg->im_msgtype],
	      msg->im_msgtype,
	      ip_hdr->ip_p,
	      fd,
	      src_str,
	      grp_str,
	      ifp->name,
	      msg->im_vif);
  }

  switch (msg->im_msgtype) {
  case IGMPMSG_WRONGVIF:
    return pim_mroute_msg_wrongvif(fd, ifp, msg);
    break;
  case IGMPMSG_NOCACHE:
    return pim_mroute_msg_nocache(fd, ifp, msg);
    break;
  case IGMPMSG_WHOLEPKT:
    return pim_mroute_msg_wholepkt(fd, ifp, (const char *)msg);
    break;
  case IGMPMSG_WRVIFWHOLE:
    return pim_mroute_msg_wrvifwhole (fd, ifp, (const char *)msg);
    break;
  default:
    break;
  }

  return 0;
}

static int mroute_read_msg(int fd)
{
  char buf[2000];
  int rd;

  rd = read(fd, buf, sizeof(buf));
  if (rd < 0) {
    zlog_warn("%s: failure reading fd=%d: errno=%d: %s",
	      __PRETTY_FUNCTION__, fd, errno, safe_strerror(errno));
    return -1;
  }

  return pim_mroute_msg(fd, buf, rd);
}

static int mroute_read(struct thread *t)
{
  int fd;
  int result;

  zassert(t);
  zassert(!THREAD_ARG(t));

  fd = THREAD_FD(t);
  zassert(fd == qpim_mroute_socket_fd);

  result = mroute_read_msg(fd);

  /* Keep reading */
  qpim_mroute_socket_reader = 0;
  mroute_read_on();

  return result;
}

static void mroute_read_on()
{
  zassert(!qpim_mroute_socket_reader);
  zassert(PIM_MROUTE_IS_ENABLED);

  THREAD_READ_ON(master, qpim_mroute_socket_reader,
		 mroute_read, 0, qpim_mroute_socket_fd);
}

static void mroute_read_off()
{
  THREAD_OFF(qpim_mroute_socket_reader);
}

int pim_mroute_socket_enable()
{
  int fd;

  if (PIM_MROUTE_IS_ENABLED)
    return -1;

  if ( pimd_privs.change (ZPRIVS_RAISE) )
    zlog_err ("pim_mroute_socket_enable: could not raise privs, %s",
              safe_strerror (errno) );

  fd = socket(AF_INET, SOCK_RAW, IPPROTO_IGMP);

  if ( pimd_privs.change (ZPRIVS_LOWER) )
    zlog_err ("pim_mroute_socket_enable: could not lower privs, %s",
	      safe_strerror (errno) );

  if (fd < 0) {
    zlog_warn("Could not create mroute socket: errno=%d: %s",
	      errno, safe_strerror(errno));
    return -2;
  }

  if (pim_mroute_set(fd, 1)) {
    zlog_warn("Could not enable mroute on socket fd=%d: errno=%d: %s",
	      fd, errno, safe_strerror(errno));
    close(fd);
    return -3;
  }

  qpim_mroute_socket_fd       = fd;

  qpim_mroute_socket_creation = pim_time_monotonic_sec();
  mroute_read_on();

  return 0;
}

int pim_mroute_socket_disable()
{
  if (PIM_MROUTE_IS_DISABLED)
    return -1;

  if (pim_mroute_set(qpim_mroute_socket_fd, 0)) {
    zlog_warn("Could not disable mroute on socket fd=%d: errno=%d: %s",
	      qpim_mroute_socket_fd, errno, safe_strerror(errno));
    return -2;
  }

  if (close(qpim_mroute_socket_fd)) {
    zlog_warn("Failure closing mroute socket: fd=%d errno=%d: %s",
	      qpim_mroute_socket_fd, errno, safe_strerror(errno));
    return -3;
  }

  mroute_read_off();
  qpim_mroute_socket_fd = -1;

  return 0;
}

/*
  For each network interface (e.g., physical or a virtual tunnel) that
  would be used for multicast forwarding, a corresponding multicast
  interface must be added to the kernel.
 */
int pim_mroute_add_vif(struct interface *ifp, struct in_addr ifaddr, unsigned char flags)
{
  struct pim_interface *pim_ifp = ifp->info;
  struct vifctl vc;
  int err;

  if (PIM_MROUTE_IS_DISABLED) {
    zlog_warn("%s: global multicast is disabled",
	      __PRETTY_FUNCTION__);
    return -1;
  }

  memset(&vc, 0, sizeof(vc));
  vc.vifc_vifi = pim_ifp->mroute_vif_index;
#ifdef VIFF_USE_IFINDEX
  vc.vifc_lcl_ifindex = ifp->ifindex;
#else
  if (ifaddr.s_addr == INADDR_ANY) {
    zlog_warn("%s: unnumbered interfaces are not supported on this platform",
	      __PRETTY_FUNCTION__);
    return -1;
  }
  memcpy(&vc.vifc_lcl_addr, &ifaddr, sizeof(vc.vifc_lcl_addr));
#endif
  vc.vifc_flags = flags;
  vc.vifc_threshold = PIM_MROUTE_MIN_TTL;
  vc.vifc_rate_limit = 0;

#ifdef PIM_DVMRP_TUNNEL  
  if (vc.vifc_flags & VIFF_TUNNEL) {
    memcpy(&vc.vifc_rmt_addr, &vif_remote_addr, sizeof(vc.vifc_rmt_addr));
  }
#endif

  err = setsockopt(qpim_mroute_socket_fd, IPPROTO_IP, MRT_ADD_VIF, (void*) &vc, sizeof(vc));
  if (err) {
    char ifaddr_str[100];

    pim_inet4_dump("<ifaddr?>", ifaddr, ifaddr_str, sizeof(ifaddr_str));

    zlog_warn("%s %s: failure: setsockopt(fd=%d,IPPROTO_IP,MRT_ADD_VIF,vif_index=%d,ifaddr=%s,flag=%d): errno=%d: %s",
	      __FILE__, __PRETTY_FUNCTION__,
	      qpim_mroute_socket_fd, ifp->ifindex, ifaddr_str, flags,
	      errno, safe_strerror(errno));
    return -2;
  }

  return 0;
}

int pim_mroute_del_vif(int vif_index)
{
  struct vifctl vc;
  int err;

  if (PIM_MROUTE_IS_DISABLED) {
    zlog_warn("%s: global multicast is disabled",
	      __PRETTY_FUNCTION__);
    return -1;
  }

  memset(&vc, 0, sizeof(vc));
  vc.vifc_vifi = vif_index;

  err = setsockopt(qpim_mroute_socket_fd, IPPROTO_IP, MRT_DEL_VIF, (void*) &vc, sizeof(vc)); 
  if (err) {
    zlog_warn("%s %s: failure: setsockopt(fd=%d,IPPROTO_IP,MRT_DEL_VIF,vif_index=%d): errno=%d: %s",
	      __FILE__, __PRETTY_FUNCTION__,
	      qpim_mroute_socket_fd, vif_index,
	      errno, safe_strerror(errno));
    return -2;
  }

  return 0;
}

int pim_mroute_add(struct channel_oil *c_oil)
{
  int err;
  int orig = 0;
  int orig_iif_vif = 0;

  qpim_mroute_add_last = pim_time_monotonic_sec();
  ++qpim_mroute_add_events;

  if (PIM_MROUTE_IS_DISABLED) {
    zlog_warn("%s: global multicast is disabled",
	      __PRETTY_FUNCTION__);
    return -1;
  }

  /* The linux kernel *expects* the incoming
   * vif to be part of the outgoing list
   * in the case of a (*,G).
   */
  if (c_oil->oil.mfcc_origin.s_addr == INADDR_ANY)
    {
      orig = c_oil->oil.mfcc_ttls[c_oil->oil.mfcc_parent];
      c_oil->oil.mfcc_ttls[c_oil->oil.mfcc_parent] = 1;
    }

  /*
   * If we have an unresolved cache entry for the S,G
   * it is owned by the pimreg for the incoming IIF
   * So set pimreg as the IIF temporarily to cause
   * the packets to be forwarded.  Then set it
   * to the correct IIF afterwords.
   */
  if (!c_oil->installed && c_oil->oil.mfcc_origin.s_addr != INADDR_ANY &&
      c_oil->oil.mfcc_parent != 0)
    {
      orig_iif_vif = c_oil->oil.mfcc_parent;
      c_oil->oil.mfcc_parent = 0;
    }
  err = setsockopt(qpim_mroute_socket_fd, IPPROTO_IP, MRT_ADD_MFC,
		   &c_oil->oil, sizeof(c_oil->oil));

  if (!err && !c_oil->installed && c_oil->oil.mfcc_origin.s_addr != INADDR_ANY &&
      orig_iif_vif != 0)
    {
      c_oil->oil.mfcc_parent = orig_iif_vif;
      err = setsockopt (qpim_mroute_socket_fd, IPPROTO_IP, MRT_ADD_MFC,
			&c_oil->oil, sizeof (c_oil->oil));
    }

  if (c_oil->oil.mfcc_origin.s_addr == INADDR_ANY)
      c_oil->oil.mfcc_ttls[c_oil->oil.mfcc_parent] = orig;

  if (err) {
    zlog_warn("%s %s: failure: setsockopt(fd=%d,IPPROTO_IP,MRT_ADD_MFC): errno=%d: %s",
	      __FILE__, __PRETTY_FUNCTION__,
	      qpim_mroute_socket_fd,
	      errno, safe_strerror(errno));
    return -2;
  }

  c_oil->installed = 1;
  return 0;
}

int pim_mroute_del (struct channel_oil *c_oil)
{
  int err;

  qpim_mroute_del_last = pim_time_monotonic_sec();
  ++qpim_mroute_del_events;

  if (PIM_MROUTE_IS_DISABLED) {
    zlog_warn("%s: global multicast is disabled",
	      __PRETTY_FUNCTION__);
    return -1;
  }

  err = setsockopt(qpim_mroute_socket_fd, IPPROTO_IP, MRT_DEL_MFC, &c_oil->oil, sizeof(c_oil->oil));
  if (err) {
    zlog_warn("%s %s: failure: setsockopt(fd=%d,IPPROTO_IP,MRT_DEL_MFC): errno=%d: %s",
	      __FILE__, __PRETTY_FUNCTION__,
	      qpim_mroute_socket_fd,
	      errno, safe_strerror(errno));
    return -2;
  }

  c_oil->installed = 0;

  return 0;
}

void
pim_mroute_update_counters (struct channel_oil *c_oil)
{
  struct sioc_sg_req sgreq;

  memset (&sgreq, 0, sizeof(sgreq));
  sgreq.src = c_oil->oil.mfcc_origin;
  sgreq.grp = c_oil->oil.mfcc_mcastgrp;

  c_oil->cc.oldpktcnt = c_oil->cc.pktcnt;
  c_oil->cc.oldbytecnt = c_oil->cc.bytecnt;
  c_oil->cc.oldwrong_if = c_oil->cc.wrong_if;
  c_oil->cc.oldlastused = c_oil->cc.lastused;

  pim_zlookup_sg_statistics (c_oil);
  if (ioctl (qpim_mroute_socket_fd, SIOCGETSGCNT, &sgreq))
    {
      char group_str[100];
      char source_str[100];

      pim_inet4_dump("<group?>", c_oil->oil.mfcc_mcastgrp, group_str, sizeof(group_str));
      pim_inet4_dump("<source?>", c_oil->oil.mfcc_origin, source_str, sizeof(source_str));

      zlog_warn ("ioctl(SIOCGETSGCNT=%lu) failure for (S,G)=(%s,%s): errno=%d: %s",
		 (unsigned long)SIOCGETSGCNT,
		 source_str,
		 group_str,
		 errno,
		 safe_strerror(errno));
      return;
    }

  c_oil->cc.pktcnt = sgreq.pktcnt;
  c_oil->cc.bytecnt = sgreq.bytecnt;
  c_oil->cc.wrong_if = sgreq.wrong_if;

  return;
}
