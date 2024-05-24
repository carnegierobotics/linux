.. SPDX-License-Identifier: GPL-2.0

=======================================================
Ambarella PCI Endpoint Function "Rainy" User Guide
=======================================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users use ambarella's Rainy endpoint function driver.

Rainy is an endpoint function aims to provide high level PCIe xfer protocol:

#. Support multiple endpoint SoCs.
#. Allows multiple apps from RC and EP get paired and transfer data, just like socket's way.

    ┌───────────────┐
    │               │
    │     EP    APP2├──────┐
    │               │      │
┌───┤APP1           │      │
│   └───────┬───────┘      │
│           │              │
│           │              │
│           │              │
│           │              │
│           │              │
│           │              │
│    ┌──────┴───────┐      │
│    │          APP2├──────┘
│    │    RC        │
└────┤APP1      APP3├──────┐
     │              │      │
     │              │      │
     └──────┬───────┘      │
            │              │
            │              │
            │              │
            │              │
            │              │
     ┌──────┴────────┐     │
     │               │     │
     │           APP3├─────┘
     │     EP        │
     │               │
     │               │
     └───────────────┘
