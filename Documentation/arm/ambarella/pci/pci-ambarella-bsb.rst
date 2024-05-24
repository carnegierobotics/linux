.. SPDX-License-Identifier: GPL-2.0

=======================================================
Ambarella PCI Endpoint Function "Bsb" User Guide
=======================================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users use Ambarella's BSB endpoint function driver.

BSB is a used to xfer bit stream buffer via PCIe DMA.

                a  ◄────┐                                                                          ┌──────────────────────────────────────────┐
                ▼       │                                                                          │               RC                         │
                b       │                                                                          │       ┌────────────────────────────┐     │
                ▼       │                                                                          │       │                            │     │
                c       │                                           ┌──────────────────────────────┼────►  │     BUFFER 1               │     │
                ▼       │                                           │                              │       │                            │     │
                d       │                                           │                              │       └────────────────────────────┘     │
                ▼       │                                           │                              │                                          │
                e       │                                           │                              │                                          │
                ▼       │                                           │                              │       ┌─────────────────────────────┐    │
                f───────┘                                           │                              │       │                             │    │
                                                                    ├──────────────────────────────┼─────► │     BUFFER 2                │    │
                                                                    │                              │       │                             │    │
                                                                    │                              │       └─────────────────────────────┘    │
                                                                    │                              │                                          │
                                                                    │                              │       ┌─────────────────────────────┐    │
                             ┌─────────────────────────┐            │                              │       │                             │    │
                             │      EP                 │            ├──────────────────────────────┼─────► │     BUFFER 3                │    │
                             │   ┌─────────────┐       │            │                              │       │                             │    │
     d.IOCTL xfer            │   │             │       │   Dmux     │                              │       └─────────────────────────────┘    │
    ┌────────────────────────┼──►│    BSB      ├───────┼────────────┤                              │                                          │
    │  rc addr, ep addr      │   │             │       │   4way     │                              │       ┌─────────────────────────────┐    │
    │  dir, size             │   │             │       │            │                              │       │                             │    │
    │                        │   │             │       │            └──────────────────────────────┼─────► │     BUFFER 4                │    │
    │                        │   └─────────────┘       │                                           │       │                             │    │
    │                        │                         │                                           │       └─────────────────────────────┘    │
    │                        │                         │                                           │                                          │
    │                        │                         │         (query info, single u32 is enough)│                                          │        ┌───────────┐
┌───┴──────┐    c. wake_up   │    ┌──────────────┐     │            write: msg_to_ep               │      ┌────────────┐       b. IOCTL,      │        │           │ 
│          │ ◄───────────────┼────┤              │◄────┼───────────────────────────────────────────┼──────┤            │◄─────────────────────┼────────┤  RC-APPs  │
│          │─────────────────┼───►│   SHM1       │     │           raise irq                       │      │    SHM1    │       wait_event     │        │  1-4      │
│ EP-APP   │ a.IOCTL:        │    │              │     │                                           │      │            │                      │        │           │ 
│          │ wait_event      │    │              │     │                                           │      │            │                      │        │           │ 
│          │                 │    └──────────────┘     │                                           │      └────────────┘                      │        │           │ 
│          │                 │                         │                                           │                                          │        │           │ 
│          │                 │                         │                                           │                                          │        │           │ 
│          │                 │                         │                                           │                                          │        │           │ 
│          │                 │                         │                                           │                                          │        │           │ 
│          │                 │    ┌───────────────┐    │         (iav bit info)                    │      ┌─────────────┐                     │        │           │
│          │ e. IOCTL        │    │               │    │            write msg_to_rc(buffer addr)   │      │             │f.wake_up correct APP│        │           │ 
│          ├─────────────────┼───►│   SHM2        ├────┼───────────────────────────────────────────┼─────►│    SHM2     ├─────────────────────┼───────►│           │ 
│          │                 │    │               │    │            raise_irq                      │      │             │                     │        │           │ 
│          │                 │    │               │    │                                           │      │             │                     │        │           │ 
└──────────┘                 │    └───────────────┘    │                                           │      └─────────────┘                     │        │           │ 
                             │                         │                                           │                                          │        └───────────┘ 
                             │                         │                                           │                                          │
                             │                         │                                           │                                          │
                             └─────────────────────────┘                                           └──────────────────────────────────────────┘




Requirements
============

BSB requires RC to has the ability to interrupt EP.
The default way is to use EP's MSI detection logic as doorbell.
But for EPs don't have MSI detection, you should use GPIO interrupt
as follows:

For RC
------

RC needs a free GPIO and cofigure to output take GPIO 45 for example:

.. code-block:: c

  bsb_rc: bsb_rc {
  	compatible = "ambarella,bsb_rc";
  	/* GPIO_ACTIVE_HIGH */
  	gpios = <&gpio 45 0>;
        ...
  };

For EP
------

EP: needs a free GPIO functioned as input, take GPIO 47 for example:

.. code-block:: c

  bsb_ep: bsb_ep {
  	compatible = "ambarella,bsb_ep";
  	interrupt-parent = <&gpio>;
  	/* RISING trigger */
  	interrupts = <47 0x1>;
        ...
  };

Setup sequence
=============
Please refer to Documentation/arm/ambarella/pci/verify-soc-interconnect-via-pci.rst

Sample Usage Guide
==================
EP-side: use :download:`epf-bsb.sh <./epf-bsb.sh>`

 .. literalinclude:: epf-bsb.sh
    :language: shell

and :download:`epf-utility.sh <./epf-utility.sh>`

 .. literalinclude:: epf-utility.sh
    :language: shell

to init bsb.

Then run bsb_ep on EP-side and run bsb_rc on RC-side.
TODO
====

#. Error handling, e.g., RC wake_up specific EP APP, but that APP is noting wait_event.
#. Introducing name service in userspace, and let it decide APP id.
