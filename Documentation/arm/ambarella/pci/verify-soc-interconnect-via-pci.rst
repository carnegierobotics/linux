.. SPDX-License-Identifier: GPL-2.0

===================================
Verify SoC interconnection via PCIe
===================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users to verify SoC interconnection via PCIe.

Currently, SoC interconnection is only verified on

* Ambarella SoC-RC <=> Ambarella SoC-EP
* PC RC <=> Ambarella SoC-EP(cv3 and cv72).

If you have verified Ambarella SoC <=> other vendors, please let us know, thanks.

Configure
=========
If you use Ambarella SoC as RC, you should:

* Make sure pcie address inside pcie dts node's "ranges" property has been reserved as no-map memory.
* reserve enough memory for bar allocation of EP. check your PCIe controller RC node's range property and EP's ``pci_epc_features->bar_fixed_size``.

If you use Ambarella SoC as EP, you should:

* reserve enough memory for EP bar allocation.

If you use other vendors' SoC/PC as EP/RC, you can refer to what Ambarella kernel/dts has already did.

We can use these two functions for verification:

#. :doc:`pci-epf-amba-test <./pci-epf-amba-test>`
#. :doc:`excalibur <./pci-ambarella-excalibur>`

* For pci_endpint_test, you should also read :doc:`../../PCI/endpoint/pci-test-howto`.
* For excalibur, you could simplely do following steps:

  * run :download:`epf-excalibur.sh <./epf-excalibur.sh>` and :download:`epf-utility.sh <./epf-utility.sh>` on EP to init epf:

    .. literalinclude:: epf-excalibur.sh
    .. literalinclude:: epf-utility.sh
       :language: shell

  * then start up RC, and run :download:`ep_excalibur.sh <./ep_excalibur.sh>` on EP kernel

    .. literalinclude:: ep_excalibur.sh
       :language: shell

  * Run :download:`rc_excalibur.sh <./rc_excalibur.sh>` on RC kernel.

    .. literalinclude:: rc_excalibur.sh
       :language: shell

Alternatively, you can also:

  * start EP
  * start RC
  * EP-side: use helper script like `epf-excalibur.sh`, `epf-moemoekyun.sh` or `epf-amba-test.sh` to initialize EPF.
  * RC-side: ``echo 1 > /sys/bus/pci/devices/0000\:00\:00.0/remove && echo 1 > /sys/bus/pci/rescan``
  * Run any codes you want.

This approach is highly recommended because the Endpoint's (EP's) PERST would be triggered during the Root Complex's (RC's) initialization, which could lead to some EP(Endpoint) registers being reset, e.g., inbound translation regs. If we establish the Endpoint Function (EPF) after the RC has booted, the concern about PERST is eliminated.

Interrupt each other
====================
PCIe spec only said EP can send INTx/MSI/MSI-x to RC, but it says nothing about "RC interrupt EP". But this is still feansible:

#. external mailbox
#. GPIO
#. software irq
#. utilize MSI detection logic for PCI/IMS.

We have MSI detection logic on cv72, allowing us to utilize it for "RC interrupt EP". However, for older chips such as cv5 and cv3, we lack MSI detection logic, and software irq cannot function through PCIe. Hence, we must resort to using GPIO: configuring RC(Any device)'s GPIO as an output and configuring EP(CV72)'s GPIO as an input.

Troubleshooting/Diagnostic
==========================

#. If you cannot see your endpoint SoC PCIe function from lspci output(e.g., 04:00.0 Class ff00: 17cd:0500), please check if your PCIe cable is correct.
   You should use a crossover cable instead of straight-through cable because crossover cable would do ``rx+ <=> tx+``, ``rx- <=> tx-``, ``tx+ <=> rx+``, ``tx- <=> rx-`` and it is required by standard PCIe protocol for data transfer.
#. When ``ln -s /sys/kernel/config/pci_ep/functions/xxx/funcx /sys/kernel/config/pci_ep/controllers/xxx.pcie``, kernel get hang: boards need rework to support
   EP mode.
#. If you use PC as RC, Ambarella SoC as EP, and PC get hang when bootup: your Ambarella board needs rework to use refclk from connector instead of its own, e.g., disconnect with EP's local clock source, and connect to connector's ``A13/A14(PCIEP1_CK_REF_P/PCIEP1_CK_REF_N)`` instead. PCIe support three different refclk design:

   * common refclk: Use RC-side clock source, which is Mostly used because it can reduce EMI
   * seperate refclk: Use EP-side clock source, and this is Ambarella EP's default behavior because our SoC also supports RC mode(and it is default role), which would use local clock source.
   * data refclk: encode clock info in tx/rx and use clock source from RC.

   Most PC only supports common refclk and doesn't consider supporting seperate refclk, so our EP SoC cannot work on them. If we rework EP board to switch to common refclk, the issue should go away. For details of rework, pls consult Ambarella people.

Introduction to memory used by RC and EP
========================================
.. list-table:: memory used by RC/EP
   :widths: 30 30 30 30
   :header-rows: 1

   * - Operations
     - src
     - dst
     - size
   * - RC OB
     - allocated by RC, e.g., dma_alloc_coherent
     - inside EP's bar
     - <= mem bar size
   * - RC IB
     - inside EP's bar
     - allocated by RC, e.g., dma_alloc_coherent
     - <= mem bar size
   * - EP OB
     - allocated by EP, e.g., dma_alloc_coherent
     - allocated by RC, e.g., dma_alloc_coherent
     - any
   * - EP IB
     - allocated by RC, e.g., dma_alloc_coherent
     - allocated by EP, e.g., dma_alloc_coherent
     - any
