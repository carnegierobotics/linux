.. SPDX-License-Identifier: GPL-2.0

=======================================================
Ambarella PCI Q&A for cadence IP core
=======================================================

:Author: Li Chen <lchen@ambarella.com>

This document is a guide to help users understand cadence IP, whic is used by Ambarella CHIPs.

Gen3 controller(cv3-dev, cv5, cv72) init sequence
=================================================
#. probe reset driver
#. execute init function in reset driver
#. cdns_torrent_phy_probe, if !already configured, deassert apb
#. ambarella_cdns_pcie_probe
#. phy init
#. phy power on: deassert link, deassert phy

Gen5 controller(cv3-dev, cv5, cv72) init sequence
=================================================
#. probe reset driver
#. execute init function in reset driver
#. cdns_excelsior_phy_probe, apb deassert , load firmware, phy deassert
#.
#. ambarella_cdns_pcie_probe
#. phy power on, link deassert

Questions from Robin Stokke
===========================

Questions
---------
We have some questions regarding the PCIe on the Ambarella CV52/CV72.

#. Endpoint interrupts: Can the PCIe root port trigger an interrupt in the endpoint function on the PCIe endpoint side? (A PCIe endpoint can always send an interrupt to the root port, but a root port can not necessarily send an interrupt to the (software defined) endpoint function; this is specific to the implementation and will typically be a "doorbell" bit that the root port can write).
#. PCIe reset pin: In the hardware programming reference manual ch. 14.5 "PCI Express Dual-Mode Controller: Initialization Sequence" - can you extend the description to also include correct behavior on the reset signal from both the root port and endpoint perspectives?
#. Inbound and outbound windows: How many inbound / outbound windows and what range are they, in root port and endpoint mode, respectively?
#. Number of endpoint functions: Can you confirm there is only room for one PCIe endpoint function?
#. Examples: Are there any endpoint mode examples in the SDK that you can recommend for our study?

Answer
------
#. Endpoint interrupts: For cv72 as EP, we can use its msi detection logic to allow RC sends doorbell. I have writen a irqchip driver here: drivers/irqchip/irq-ambarella-msi-detection-doorbell.c. You may see this platform msi driver for next sdk release. For cv52 as EP, you can use RC and EP's free GPIO for this purpose.
#. PCIe reset pin: You can refer to drivers/reset/reset-amba-cdns-phy.c
#. Inbound and outbound windows. We utilize the cadence PCIe IP core, which offers 8 outbound windows/regions, but does not provide any inbound windows. In the role of RC, these windows are employed for mapping the bar of the RP and the configuration mapping of the EP. Conversely, in the role of EP, these windows are used for mapping the addresses of the RC.
#. Number of endpoint functions. both cv52 and cv72 only support single PF.
#. Examples: I wrote some EPF drivers in Linux kernel, you can find them under drivers/pci/endpoint/functions and drivers/soc/ambarella.
