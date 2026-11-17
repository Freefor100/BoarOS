/**
 * @file
 * Multicast listener discovery
 *
 * @defgroup mld6 MLD6
 * @ingroup ip6
 * Multicast listener discovery for IPv6. Aims to be compliant with RFC 2710.
 * No support for MLDv2.<br>
 * Note: The allnodes (ff01::1, ff02::1) group is assumed be received by your
 * netif since it must always be received for correct IPv6 operation (e.g. SLAAC).
 * Ensure the netif filters are configured accordingly!<br>
 * The netif flags also need NETIF_FLAG_MLD6 flag set to enable MLD6 on a
 * netif ("netif->flags |= NETIF_FLAG_MLD6;").<br>
 * To be called from TCPIP thread.
 */

/*
 * Copyright (c) 2010 Inico Technologies Ltd.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without modification,
 * are permitted provided that the following conditions are met:
 *
 * 1. Redistributions of source code must retain the above copyright notice,
 *    this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright notice,
 *    this list of conditions and the following disclaimer in the documentation
 *    and/or other materials provided with the distribution.
 * 3. The name of the author may not be used to endorse or promote products
 *    derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT
 * SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT
 * OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING
 * IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY
 * OF SUCH DAMAGE.
 *
 * This file is part of the lwIP TCP/IP stack.
 *
 * Author: Ivan Delamer <delamer@inicotech.com>
 *
 *
 * Please coordinate changes and requests with Ivan Delamer
 * <delamer@inicotech.com>
 */

/* Based on igmp.c implementation of igmp v2 protocol */

#include "lwip/opt.h"

#if LWIP_IPV6 && LWIP_IPV6_MLD  /* don't build if not configured for use in lwipopts.h */

#include "lwip/mld6.h"
#include "lwip/prot/mld6.h"
#include "lwip/icmp6.h"
#include "lwip/ip6.h"
#include "lwip/ip6_addr.h"
#include "lwip/ip.h"
#include "lwip/inet_chksum.h"
#include "lwip/pbuf.h"
#include "lwip/netif.h"
#include "lwip/memp.h"
#include "lwip/stats.h"
#include "lwip/timeouts.h"

#include <string.h>


/*
 * MLD constants
 */
#define MLD6_HL                           1
#define MLD6_JOIN_DELAYING_MEMBER_TMR_MS  (500)

#define MLD6_GROUP_NON_MEMBER             0
#define MLD6_GROUP_DELAYING_MEMBER        1
#define MLD6_GROUP_IDLE_MEMBER            2

/* Forward declarations. */
static struct mld_group *mld6_new_group(struct netif *ifp, const ip6_addr_t *addr);
static err_t mld6_remove_group(struct netif *netif, struct mld_group *group);
static void mld6_delayed_report(struct mld_group *group, u16_t maxresp);
static err_t mld6_send(struct netif *netif, struct mld_group *group, u8_t type);

/* RFC2710 excludes scope zero/one and link-local all-nodes from reports. */
static int
mld6_reportable(const ip6_addr_t *address)
{
  return ip6_addr_ismulticast(address) && ip6_addr_multicast_scope(address)>1 &&
    !ip6_addr_isallnodes_linklocal(address);
}

static int
mld6_link_ready(const struct netif *netif)
{
  return netif->hwaddr_len==6 && netif_is_up(netif) &&
    netif_is_link_up(netif) && netif->mtu>=1280;
}

/* One owner timeout while reports are pending, not a permanent 10Hz scan.
 * The timeout holds no netif/group pointer, so removal needs no cancellation. */
static u8_t mld6_timer_running;
static void mld6_timeout(void *arg);
static void
mld6_timer_needed(void)
{
  struct netif *netif;
  if(mld6_timer_running)return;
  NETIF_FOREACH(netif) {
    struct mld_group *group;
    if(!mld6_link_ready(netif))continue;
    for(group=netif_mld6_data(netif);group;group=group->next) {
      if(group->group_state==MLD6_GROUP_DELAYING_MEMBER && group->timer) {
        mld6_timer_running=1;
        sys_timeout(MLD6_TMR_INTERVAL,mld6_timeout,NULL);
        return;
      }
    }
  }
}
static void
mld6_timeout(void *arg)
{
  LWIP_UNUSED_ARG(arg);
  mld6_timer_running=0;
  mld6_tmr();
  mld6_timer_needed();
}

/* Queries may require a response before the next 100ms timer tick. Keep
 * failure retry on that existing timer rather than dropping the obligation. */
static void
mld6_query_report(struct netif *netif,struct mld_group *group,u16_t maximum)
{
  if(group->group_state==MLD6_GROUP_DELAYING_MEMBER && group->timer &&
     maximum>=(u32_t)group->timer*MLD6_TMR_INTERVAL)return;
  u16_t ticks=maximum/MLD6_TMR_INTERVAL;
#ifdef LWIP_RAND
  ticks=(u16_t)(LWIP_RAND()%((u32_t)ticks+1));
#endif
  if(!ticks) {
    if(mld6_send(netif,group,ICMP6_TYPE_MLR)==ERR_OK) {
      MLD6_STATS_INC(mld6.tx_report);
      group->timer=0;group->group_state=MLD6_GROUP_IDLE_MEMBER;
    } else {
      group->timer=1;group->group_state=MLD6_GROUP_DELAYING_MEMBER;
    }
  } else if(group->group_state==MLD6_GROUP_IDLE_MEMBER || !group->timer || ticks<group->timer) {
    group->timer=ticks;group->group_state=MLD6_GROUP_DELAYING_MEMBER;
  }
  mld6_timer_needed();
}


/**
 * Stop MLD processing on interface
 *
 * @param netif network interface on which stop MLD processing
 */
err_t
mld6_stop(struct netif *netif)
{
  struct mld_group *group = netif_mld6_data(netif);

  netif_set_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_MLD6, NULL);

  while (group != NULL) {
    struct mld_group *next = group->next; /* avoid use-after-free below */

    /* disable the group at the MAC level */
    if (netif->mld_mac_filter != NULL) {
      netif->mld_mac_filter(netif, &(group->group_address), NETIF_DEL_MAC_FILTER);
    }

    /* free group */
    memp_free(MEMP_MLD6_GROUP, group);

    /* move to "next" */
    group = next;
  }
  return ERR_OK;
}

/**
 * Report MLD memberships for this interface
 *
 * @param netif network interface on which report MLD memberships
 */
void
mld6_report_groups(struct netif *netif)
{
  if(!netif->hwaddr_len)return; /* Loopback memberships need no wire protocol. */
  struct mld_group *group = netif_mld6_data(netif);

  while (group != NULL) {
    if(mld6_reportable(&group->group_address))
      mld6_delayed_report(group, MLD6_JOIN_DELAYING_MEMBER_TMR_MS);
    group = group->next;
  }
}

/**
 * Search for a group that is joined on a netif
 *
 * @param ifp the network interface for which to look
 * @param addr the group ipv6 address to search for
 * @return a struct mld_group* if the group has been found,
 *         NULL if the group wasn't found.
 */
struct mld_group *
mld6_lookfor_group(struct netif *ifp, const ip6_addr_t *addr)
{
  struct mld_group *group = netif_mld6_data(ifp);

  while (group != NULL) {
    if (ip6_addr_eq(&(group->group_address), addr)) {
      return group;
    }
    group = group->next;
  }

  return NULL;
}


/**
 * create a new group
 *
 * @param ifp the network interface for which to create
 * @param addr the new group ipv6
 * @return a struct mld_group*,
 *         NULL on memory error.
 */
static struct mld_group *
mld6_new_group(struct netif *ifp, const ip6_addr_t *addr)
{
  struct mld_group *group;

  group = (struct mld_group *)memp_malloc(MEMP_MLD6_GROUP);
  if (group != NULL) {
    ip6_addr_set(&(group->group_address), addr);
    group->timer              = 0; /* Not running */
    group->group_state        = MLD6_GROUP_IDLE_MEMBER;
    group->last_reporter_flag = 0;
    group->use                = 0;
    group->next               = netif_mld6_data(ifp);

    netif_set_client_data(ifp, LWIP_NETIF_CLIENT_DATA_INDEX_MLD6, group);
  }

  return group;
}

/**
 * Remove a group from the mld_group_list, but do not free it yet
 *
 * @param group the group to remove
 * @return ERR_OK if group was removed from the list, an err_t otherwise
 */
static err_t
mld6_remove_group(struct netif *netif, struct mld_group *group)
{
  err_t err = ERR_OK;

  /* Is it the first group? */
  if (netif_mld6_data(netif) == group) {
    netif_set_client_data(netif, LWIP_NETIF_CLIENT_DATA_INDEX_MLD6, group->next);
  } else {
    /* look for group further down the list */
    struct mld_group *tmpGroup;
    for (tmpGroup = netif_mld6_data(netif); tmpGroup != NULL; tmpGroup = tmpGroup->next) {
      if (tmpGroup->next == group) {
        tmpGroup->next = group->next;
        break;
      }
    }
    /* Group not find group */
    if (tmpGroup == NULL) {
      err = ERR_ARG;
    }
  }

  return err;
}


/**
 * Process an input MLD message. Called by icmp6_input.
 *
 * @param p the mld packet, p->payload pointing to the icmpv6 header
 * @param inp the netif on which this packet was received
 */
void
mld6_input(struct pbuf *p, struct netif *inp)
{
  struct mld_header header;
  ip6_addr_t address;
  struct mld_group *group;
  MLD6_STATS_INC(mld6.recv);
  /* ICMPv6 already checked the entire checksum. Copy only the fixed header,
   * including when it straddles pbufs; RFC2710 requires ignoring trailing data
   * and Code/Reserved rather than treating them as malformed. */
  if(p->tot_len<sizeof(header) || pbuf_copy_partial(p,&header,sizeof(header),0)!=sizeof(header)) {
    MLD6_STATS_INC(mld6.lenerr);goto drop;
  }
  if(!mld6_link_ready(inp) || !boaros_lwip_ip6_mld_valid_header() ||
     !ip6_addr_islinklocal(ip6_current_src_addr()))goto invalid;
  ip6_addr_copy_from_packed(address,header.multicast_address);
  ip6_addr_assign_zone(&address,IP6_MULTICAST,inp);
  switch(header.type) {
  case ICMP6_TYPE_MLQ:
    if(ip6_addr_isany(&address)) {
      if(!ip6_addr_isallnodes_linklocal(ip6_current_dest_addr()))goto invalid;
      MLD6_STATS_INC(mld6.rx_general);
      for(group=netif_mld6_data(inp);group;group=group->next)
        if(mld6_reportable(&group->group_address))
          mld6_query_report(inp,group,lwip_ntohs(header.max_resp_delay));
    } else {
      if(!mld6_reportable(&address) || !ip6_addr_eq(&address,ip6_current_dest_addr()))goto invalid;
      MLD6_STATS_INC(mld6.rx_group);
      group=mld6_lookfor_group(inp,&address);
      if(group)mld6_query_report(inp,group,lwip_ntohs(header.max_resp_delay));
    }
    break;
  case ICMP6_TYPE_MLR:
    if(!mld6_reportable(&address) || !ip6_addr_eq(&address,ip6_current_dest_addr()))goto invalid;
    MLD6_STATS_INC(mld6.rx_report);
    group=mld6_lookfor_group(inp,&address);
    if(group) {
      group->timer=0;group->group_state=MLD6_GROUP_IDLE_MEMBER;
      group->last_reporter_flag=0;
    }
    break;
  case ICMP6_TYPE_MLD:
    if(!mld6_reportable(&address) || !ip6_addr_isallrouters_linklocal(ip6_current_dest_addr()))goto invalid;
    /* Host-only implementation. A router may query the remaining listeners. */
    break;
  default:goto invalid;
  }
  pbuf_free(p);return;
invalid:
  MLD6_STATS_INC(mld6.proterr);
drop:
  MLD6_STATS_INC(mld6.drop);pbuf_free(p);
}

/**
 * @ingroup mld6
 * Join a group on one or all network interfaces.
 *
 * If the group is to be joined on all interfaces, the given group address must
 * not have a zone set (i.e., it must have its zone index set to IP6_NO_ZONE).
 * If the group is to be joined on one particular interface, the given group
 * address may or may not have a zone set.
 *
 * @param srcaddr ipv6 address (zoned) of the network interface which should
 *                join a new group. If IP6_ADDR_ANY6, join on all netifs
 * @param groupaddr the ipv6 address of the group to join (possibly but not
 *                  necessarily zoned)
 * @return ERR_OK if group was joined on the netif(s), an err_t otherwise
 */
err_t
mld6_joingroup(const ip6_addr_t *srcaddr, const ip6_addr_t *groupaddr)
{
  err_t         err = ERR_VAL; /* no matching interface */
  struct netif *netif;

  LWIP_ASSERT_CORE_LOCKED();

  /* loop through netif's */
  NETIF_FOREACH(netif) {
    /* Should we join this interface ? */
    if (ip6_addr_isany(srcaddr) ||
        netif_get_ip6_addr_match(netif, srcaddr) >= 0) {
      err = mld6_joingroup_netif(netif, groupaddr);
      if (err != ERR_OK) {
        return err;
      }
    }
  }

  return err;
}

/**
 * @ingroup mld6
 * Join a group on a network interface.
 *
 * @param netif the network interface which should join a new group.
 * @param groupaddr the ipv6 address of the group to join (possibly but not
 *                  necessarily zoned)
 * @return ERR_OK if group was joined on the netif, an err_t otherwise
 */
err_t
mld6_joingroup_netif(struct netif *netif, const ip6_addr_t *groupaddr)
{
  struct mld_group *group;
#if LWIP_IPV6_SCOPES
  ip6_addr_t ip6addr;

  /* If the address has a particular scope but no zone set, use the netif to
   * set one now. Within the mld6 module, all addresses are properly zoned. */
  if (ip6_addr_lacks_zone(groupaddr, IP6_MULTICAST)) {
    ip6_addr_set(&ip6addr, groupaddr);
    ip6_addr_assign_zone(&ip6addr, IP6_MULTICAST, netif);
    groupaddr = &ip6addr;
  }
  IP6_ADDR_ZONECHECK_NETIF(groupaddr, netif);
#endif /* LWIP_IPV6_SCOPES */

  LWIP_ASSERT_CORE_LOCKED();

  if(!ip6_addr_ismulticast(groupaddr))return ERR_VAL;
  /* find group or create a new one if not found */
  group = mld6_lookfor_group(netif, groupaddr);
  if(group && group->use==UINT32_MAX)return ERR_MEM;

  if (group == NULL) {
    /* Joining a new group. Create a new group entry. */
    group = mld6_new_group(netif, groupaddr);
    if (group == NULL) {
      return ERR_MEM;
    }

    /* Activate this address on the MAC layer. */
    if (netif->mld_mac_filter != NULL) {
      err_t error=netif->mld_mac_filter(netif, groupaddr, NETIF_ADD_MAC_FILTER);
      if(error!=ERR_OK) {
        mld6_remove_group(netif,group);memp_free(MEMP_MLD6_GROUP,group);return error;
      }
    }

    /* Report our membership. */
    if(netif->hwaddr_len && mld6_reportable(groupaddr)) {
      if(mld6_send(netif,group,ICMP6_TYPE_MLR)==ERR_OK)MLD6_STATS_INC(mld6.tx_report);
      mld6_delayed_report(group,MLD6_JOIN_DELAYING_MEMBER_TMR_MS);
    }
  }

  /* Increment group use */
  group->use++;
  return ERR_OK;
}

/**
 * @ingroup mld6
 * Leave a group on a network interface.
 *
 * Zoning of address follows the same rules as @ref mld6_joingroup.
 *
 * @param srcaddr ipv6 address (zoned) of the network interface which should
 *                leave the group. If IP6_ADDR_ANY6, leave on all netifs
 * @param groupaddr the ipv6 address of the group to leave (possibly, but not
 *                  necessarily zoned)
 * @return ERR_OK if group was left on the netif(s), an err_t otherwise
 */
err_t
mld6_leavegroup(const ip6_addr_t *srcaddr, const ip6_addr_t *groupaddr)
{
  err_t         err = ERR_VAL; /* no matching interface */
  struct netif *netif;

  LWIP_ASSERT_CORE_LOCKED();

  /* loop through netif's */
  NETIF_FOREACH(netif) {
    /* Should we leave this interface ? */
    if (ip6_addr_isany(srcaddr) ||
        netif_get_ip6_addr_match(netif, srcaddr) >= 0) {
      err_t res = mld6_leavegroup_netif(netif, groupaddr);
      if (err != ERR_OK) {
        /* Store this result if we have not yet gotten a success */
        err = res;
      }
    }
  }

  return err;
}

/**
 * @ingroup mld6
 * Leave a group on a network interface.
 *
 * @param netif the network interface which should leave the group.
 * @param groupaddr the ipv6 address of the group to leave (possibly, but not
 *                  necessarily zoned)
 * @return ERR_OK if group was left on the netif, an err_t otherwise
 */
static err_t
mld6_leavegroup_netif_mode(struct netif *netif, const ip6_addr_t *groupaddr,int force)
{
  struct mld_group *group;
#if LWIP_IPV6_SCOPES
  ip6_addr_t ip6addr;

  if (ip6_addr_lacks_zone(groupaddr, IP6_MULTICAST)) {
    ip6_addr_set(&ip6addr, groupaddr);
    ip6_addr_assign_zone(&ip6addr, IP6_MULTICAST, netif);
    groupaddr = &ip6addr;
  }
  IP6_ADDR_ZONECHECK_NETIF(groupaddr, netif);
#endif /* LWIP_IPV6_SCOPES */

  LWIP_ASSERT_CORE_LOCKED();

  /* find group */
  group = mld6_lookfor_group(netif, groupaddr);

  if (group != NULL) {
    /* Leave if there is no other use of the group */
    if (group->use <= 1) {
      if(netif->mld_mac_filter) {
        err_t error=netif->mld_mac_filter(netif,groupaddr,NETIF_DEL_MAC_FILTER);
        /* Final-owner teardown cannot retain an unowned software reference.
         * A stale NIC acceptance entry is still rejected by the IP group gate. */
        if(error!=ERR_OK && !force)return error;
      }
      /* Remove the group from the list */
      mld6_remove_group(netif, group);

      /* If we are the last reporter for this group */
      if (group->last_reporter_flag) {
        MLD6_STATS_INC(mld6.tx_leave);
        mld6_send(netif, group, ICMP6_TYPE_MLD);
      }

      /* free group struct */
      memp_free(MEMP_MLD6_GROUP, group);
    } else {
      /* Decrement group use */
      group->use--;
    }

    /* Left group */
    return ERR_OK;
  }

  /* Group not found */
  return ERR_VAL;
}


err_t
mld6_leavegroup_netif(struct netif *netif,const ip6_addr_t *groupaddr)
{
  return mld6_leavegroup_netif_mode(netif,groupaddr,0);
}

err_t
mld6_leavegroup_netif_force(struct netif *netif,const ip6_addr_t *groupaddr)
{
  return mld6_leavegroup_netif_mode(netif,groupaddr,1);
}

/**
 * Periodic timer for mld processing. Must be called every
 * MLD6_TMR_INTERVAL milliseconds (100).
 *
 * When a delaying member expires, a membership report is sent.
 */
void
mld6_tmr(void)
{
  struct netif *netif;

  NETIF_FOREACH(netif) {
    struct mld_group *group = netif_mld6_data(netif);
    if(!mld6_link_ready(netif))continue;

    while (group != NULL) {
      if (group->timer > 0) {
        group->timer--;
        if (group->timer == 0) {
          /* If the state is MLD6_GROUP_DELAYING_MEMBER then we send a report for this group */
          if (group->group_state == MLD6_GROUP_DELAYING_MEMBER) {
            if(mld6_send(netif,group,ICMP6_TYPE_MLR)==ERR_OK) {
              MLD6_STATS_INC(mld6.tx_report);
              group->group_state=MLD6_GROUP_IDLE_MEMBER;
            } else group->timer=1;
          }
        }
      }
      group = group->next;
    }
  }
}

/**
 * Schedule a delayed membership report for a group
 *
 * @param group the mld_group for which "delaying" membership report
 *              should be sent
 * @param maxresp_in the max resp delay provided in the query
 */
static void
mld6_delayed_report(struct mld_group *group, u16_t maxresp_in)
{
  /* Convert maxresp from milliseconds to tmr ticks */
  u16_t maxresp = maxresp_in / MLD6_TMR_INTERVAL;
  if (maxresp == 0) {
    maxresp = 1;
  }

#ifdef LWIP_RAND
  /* Randomize maxresp. (if LWIP_RAND is supported) */
  maxresp = (u16_t)(LWIP_RAND() % maxresp);
  if (maxresp == 0) {
    maxresp = 1;
  }
#endif /* LWIP_RAND */

  /* Apply timer value if no report has been scheduled already. */
  if ((group->group_state == MLD6_GROUP_IDLE_MEMBER) ||
     ((group->group_state == MLD6_GROUP_DELAYING_MEMBER) &&
      ((group->timer == 0) || (maxresp < group->timer)))) {
    group->timer = maxresp;
    group->group_state = MLD6_GROUP_DELAYING_MEMBER;
  }
  mld6_timer_needed();
}

/**
 * Send a MLD message (report or done).
 *
 * An IPv6 hop-by-hop options header with a router alert option
 * is prepended.
 *
 * @param group the group to report or quit
 * @param type ICMP6_TYPE_MLR (report) or ICMP6_TYPE_MLD (done)
 */
static err_t
mld6_send(struct netif *netif, struct mld_group *group, u8_t type)
{
  struct mld_header *mld_hdr;
  struct pbuf *p;
  const ip6_addr_t *src_addr,*destination;
  ip6_addr_t routers;
  if(!mld6_reportable(&group->group_address) || !netif->hwaddr_len)return ERR_OK;
  if(!mld6_link_ready(netif))return ERR_RTE;
  ip6_addr_set_allrouters_linklocal(&routers);
  ip6_addr_assign_zone(&routers,IP6_MULTICAST,netif);
  destination=type==ICMP6_TYPE_MLD?&routers:&group->group_address;

  /* Allocate a packet. Size is MLD header + IPv6 Hop-by-hop options header. */
  p = pbuf_alloc(PBUF_IP, sizeof(struct mld_header) + MLD6_HBH_HLEN, PBUF_RAM);
  if (p == NULL) {
    MLD6_STATS_INC(mld6.memerr);
    return ERR_MEM;
  }

  /* Move to make room for Hop-by-hop options header. */
  if (pbuf_remove_header(p, MLD6_HBH_HLEN)) {
    pbuf_free(p);
    MLD6_STATS_INC(mld6.lenerr);
    return ERR_BUF;
  }

  /* Select our source address. */
  if (!ip6_addr_isvalid(netif_ip6_addr_state(netif, 0))) {
    /* This is a special case, when we are performing duplicate address detection.
     * We must join the multicast group, but we don't have a valid address yet. */
    src_addr = IP6_ADDR_ANY6;
  } else {
    /* Use link-local address as source address. */
    src_addr = netif_ip6_addr(netif, 0);
  }

  /* MLD message header pointer. */
  mld_hdr = (struct mld_header *)p->payload;

  /* Set fields. */
  mld_hdr->type = type;
  mld_hdr->code = 0;
  mld_hdr->chksum = 0;
  mld_hdr->max_resp_delay = 0;
  mld_hdr->reserved = 0;
  ip6_addr_copy_to_packed(mld_hdr->multicast_address, group->group_address);

#if CHECKSUM_GEN_ICMP6
  IF__NETIF_CHECKSUM_ENABLED(netif, NETIF_CHECKSUM_GEN_ICMP6) {
    mld_hdr->chksum = ip6_chksum_pseudo(p, IP6_NEXTH_ICMP6, p->len,
      src_addr, destination);
  }
#endif /* CHECKSUM_GEN_ICMP6 */

  /* Add hop-by-hop headers options: router alert with MLD value. */
  ip6_options_add_hbh_ra(p, IP6_NEXTH_ICMP6, IP6_ROUTER_ALERT_VALUE_MLD);

  /* A failed TX must not claim last-reporter ownership. */
  err_t result=ip6_output_if(p,(ip6_addr_isany(src_addr))?NULL:src_addr,destination,
      MLD6_HL,0,IP6_NEXTH_HOPBYHOP,netif);
  if(result==ERR_OK) {
    MLD6_STATS_INC(mld6.xmit);
    if(type==ICMP6_TYPE_MLR)group->last_reporter_flag=1;
  }
  pbuf_free(p);
  return result;
}

#endif /* LWIP_IPV6 */
