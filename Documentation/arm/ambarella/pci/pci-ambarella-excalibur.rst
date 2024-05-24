.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella PCI Endpoint Function "Excalibur" User Guide
======================================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users use ambarella's Excalibur endpoint function driver.

Excalibur is a full-featured endpoint function.

Prerequisites
=============
Before using this epf, please make sure your EP-mode controller works correctly.
You can use `pci_endpoint_test.c` and its friends to test.

What does Excalibur provide
===========================
* xfer between rc and ep: RC can read/write EP, and EP can read/write RC.
* use ``gen_pool`` for memory pool, so you can alloc/free from both rc's buffer/ep's mem bar.
* use ``gen_pool`` to manage ep's mem bar, so you don't need to know EP PCIe bar details
* checksum(optional at runtime, controlled by module paramenter from sysfs)
* Both RC and EP are aware of when RC|EP IB|OB completes(via poll or dma irq)
* rate calc.

Init sequence
=============
Please refer to Documentation/arm/ambarella/pci/verify-soc-interconnect-via-pci.rst

Guide for Sample Use
====================
EP-side: use :download:`epf-excalibur.sh <./epf-excalibur.sh>`

 .. literalinclude:: epf-excalibur.sh
    :language: shell

and :download:`epf-utility.sh <./epf-utility.sh>`

 .. literalinclude:: epf-utility.sh
    :language: shell

to init excalibur.

* enable checksum: ``echo Y /sys/module/ambarella_excalibur_rc/enable_checksum`` on RC and  ``echo Y /sys/module/ambarella_excalibur_ep/enable_checksum`` on EP(Note: ``enable_checksum`` is only for test/debug purpose)

* get throughput: ``echo Y /sys/module/ambarella_excalibur_rc/calc_rate`` on RC and ``echo Y /sys/module/ambarella_excalibur_ep/calc_rate`` on EP(Note: ``calc_rate`` is only for test)

see other parameters inside ``/sys/module/ambarella_excalibur_rc/*`` and ``/sys/module/ambarella_excalibur_ep/*``

* Enable ``CONFIG_SAMPLE_AMBARELLA_EXCALIBUR_EP`` and ``CONFIG_SAMPLE_AMBARELLA_EXCALIBUR_RC`` if you want to run tests. For example, use `ep_excalibur.sh` and `ep_excalibur.sh` for testing purposes.

one test round
--------------
Take ep ob for example,

#. RC: ``modprobe excalibur_RC_ep_ob``
#. EP: ``modprobe excalibur_EP_ep_ob``

Many rounds of testing
--------------------------
#. Run :download:`ep_excalibur.sh <./ep_excalibur.sh>` on EP kernel.

 .. literalinclude:: ep_excalibur.sh
    :language: shell

#. Run :download:`rc_excalibur.sh <./rc_excalibur.sh>` on RC kernel

 .. literalinclude:: ep_excalibur.sh
    :language: shell

By default, these two scripts will continuously run 1000000 times.
However, you can also specify the number of times you want them to run, such as ``rc_excalibur.sh 10`` and ``ep_excalibur.sh 10``, which would run each script 10 times.

Current Status
==============
- [x] single EP SoC: non-parallel RC OB
- [x] single EP SoC: non-parallel RC IB
- [x] single EP SoC: non-parallel EP OB
- [x] single EP SoC: non-parallel EP IB
- [ ] single EP SoC: parallel EP IB
- [ ] single EP SoC: parallel RC OB
- [ ] single EP SoC: parallel RC IB
- [ ] single EP SoC: parallel EP OB
- [ ] single EP SoC: parallel EP IB
- [ ] single EP SoC: mix parallel EP|RC IB|OB
- [ ] dual EP SoCs:  ...

Design Notes
============

Never use completion for cross-chip events, e.g.,
-------------------------------------------------
#. RC: setup CMD in reg bar, and ``wait_for_completion`` ``A``.
#. EP: poll something and get the CMD, do what it should do. then send a CMD to RC(via irq)
#. RC: complete ``A`` in irq's bottomhalf.

This workflow will get over-complex when more and more CMDs come into play.

To simplify design/implementation, we can just replace interrupt with poll like ``readl_poll_timeout``, no irq,
no complete.

But we can consider using complete if no remote SoC is involved in the event, like calc/print xfer rate.

Throughput Tunning
==================
* decrease ``poll_delay_us`` to reduce latency, but if loop too tight, we may get Async Serror. So please test fully.
* check ``/sys/module/ambarella_cdns_udma/parameters/enable_mrrs_quirk``, this option is enabled by default on cv3 and cv5 because if PC roled as RC, EP udma ib may get corrupt data on some PCIe slots. you can disable it if you don't have PC as RC or your PC slot cannot reproduce this issue(you can also use pcitest to check).

How to debug excalibur
======================
* Both excalibur_ep and excalibur_rc provides the same module arguments. you could use ``dump_buffer/silence_checksum/poll_delay_us/poll_timeout/debug_poll`` to debug your issues, e.g.

   ::

     echo Y > /sys/module/ambarella_excalibur_ep/parameters/debug_poll
     echo Y > /sys/module/ambarella_excalibur_rc/parameters/poll_timeout

* enable ``CONFIG_DMADEVICES_DEBUG`` and ``CONFIG_DMADEVICES_VDEBUG``
