.. SPDX-License-Identifier: GPL-2.0

=======================================================
Ambarella PCI Endpoint Function "Moemoekyun" User Guide
=======================================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users use ambarella's Moemoekyun endpoint function driver.

Moemoekyun is a minimized & straightforward endpoint function and api-provider.

Init sequence
=============
Please refer to Documentation/arm/ambarella/pci/verify-soc-interconnect-via-pci.rst

Sample Usage Guide
==================
EP-side: use :download:`epf-moemoekyun.sh <./epf-moemoekyun.sh>`

 .. literalinclude:: epf-moemoekyun.sh
    :language: shell

and :download:`epf-utility.sh <./epf-utility.sh>`

 .. literalinclude:: epf-utility.sh
    :language: shell

to init moemoekyun.

RC ob:

#. ``modprobe moemoekyun_RC_rc_ob``: RC should write to EP's mem bar, which is fixed: ``[endpoints_info->ep_mem_pci_addr, endpoints_info->ep_mem_pci_addr+ep_mem_pci_addr)``.
#. ``modprobe moemoekyun_EP_rc_ob``: EP should read from ``[moemoekyun_ep->mem_bar_base, moemoekyun_ep->mem_bar_base)``.
#. see `moemoekyun_RC_rc_ob.c` and `moemoekyun_EP_rc_ob.c` for reference

RC ib:

#. ``modprobe moemoekyun_EP_rc_ib:`` EP should write to ``[moemoekyun_ep->mem_bar_base, moemoekyun_ep->mem_bar_base)``
#. ``modprobe moemoekyun_RC_rc_ib:`` RC should read from EP's mem bar, which is fixed: ``[endpoints_info->ep_mem_pci_addr, endpoints_info->ep_mem_pci_addr+ep_mem_pci_addr)``
#. see `moemoekyun_RC_rc_ib.c` and `moemoekyun_EP_rc_ib.c` for reference

EP ib:

#. RC buffer is not fixed, so you should firstly use ``moemoekyun_rc_tell_ep_dma_range`` to tell EP about RC's buffer base dma_addr and size.
#. EP use ``moemoekyun_get_rc_dma_addr`` and ``moemoekyun_get_rc_dma_region_size`` to get RC buffer's base addr and size.
#. see `moemoekyun_RC_ep_ib.c` and `moemoekyun_EP_ep_ib.c` for reference

EP ob:

#. RC buffer is not fixed, so you should firstly use ``moemoekyun_rc_tell_ep_dma_range`` to tell EP about RC's buffer base dma_addr and size.
#. EP use ``moemoekyun_get_rc_dma_addr`` and ``moemoekyun_get_rc_dma_region_size`` to get RC buffer's base addr and size.
#. see `moemoekyun_RC_ep_ob.c` and `moemoekyun_EP_ep_ob.c` for reference
