#include <zebra.h>

#include "if.h"
#include "zebra/debug.h"
#include "zebra/zserv.h"
#include "zebra/rib.h"
#include "zebra/zebra_vrf.h"
#include "zebra/zebra_l2.h"

int zebra_l2_bridge_add_update (struct interface *ifp,
                                struct zebra_l2if_bridge *zl2if)
{
  return 0;
}

int zebra_l2_vlanif_add_update (struct interface *ifp,
                                struct zebra_l2if_vlan *zl2if)
{
  return 0;
}

int zebra_l2_bridge_del (struct interface *ifp)
{
  return 0;
}

int zebra_l2_vlanif_del (struct interface *ifp)
{
  return 0;
}
