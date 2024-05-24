.. SPDX-License-Identifier: GPL-2.0

=======================================
PCI Endpoint Function "amba-test" User Guide
=======================================

:Author: Li Chen <lchen@ambarella.com>

This document provides guidance for users looking to make use of the `pci-epf-amba-test.c` and `pci_endpoint_amba_test.c drivers.`

`pci-epf-amba-test.c` serves as an endpoint function driver that sets up an Enhanced PCI Endpoint Function (EPF)
driver named ``"pci-epf-amba-test"`` on the Endpoint (EP) side. This driver, along with its counterpart, has been adapted
from the kernel's pci-epf-test driver to offer a more feature-rich and optimized functionality.

Init sequence
=============
Please refer to Documentation/arm/ambarella/pci/verify-soc-interconnect-via-pci.rst

Sample Usage Guide
==================

* Users should enable the ``BUILD_AMBARELLA_UNIT_TESTS_PCI_EPF`` option in both the EP and RC SDKs before starting the build process.
* For SoCs run as EP, you should enable ``CONFIG_AMBOOT_PCIE[0-2]?_MODE_EP``.
* For SoCs run as RC, you should enable ``CONFIG_AMBOOT_PCIE[0-2]?_MODE_RC``.

check if EPF matched
--------------------
You should check if pci_end_amba_test is matched

#. ``modprobe pci_endpoint_amba_test`` if it is not builtin.

   ::

    # modprobe pci_endpoint_amba_test
    pci-endpoint-amba-test 0000:04:00.0: enabling device (0000 -> 0002)

#. Ensure that lspci -k can detect your Endpoint (EP) device and the associated EPF driver.

   ::

     # lspci -k
     04:00.0 Class ff00: 17cd:0500 pci-endpoint-amba-test

re-initialize bar on EP-side(optional)
----------------------------------------
For certain reasons, you need to re-initialize the EP's BAR configurations after the RC boots up if your epf is initialized after RC bootup, especially if the EP is a CV72:

 ::
    echo 1 > /sys/kernel/config/pci_ep/functions/pci_epf_amba_test/func0/setbar

Run your tests
---------------------------------
Run ``amba-pcitest`` on the RC-side. This provides various basic tests similar to pcitest, including bar, dma, throughput, and irq tests.
Additionally, it offers advanced features and options, such as data split, throughput, dma/non-dma, dma_alloc_coherent/dma_map*, and more.

Here is a simple example that demonstrates how to obtain EP PCIe uDMA throughput measurements(Note: run it on RC):

 ::
    amba-pcitest -d -w -r -s 0x200000

Once the example is executed, you can expect to see results from EP similar to the following(Note: this example uses cv72 ga as EP and cv5 timn as RC):

 ::
    WRITE => Size: 2097152 bytes      DMA: dma_map_*  Time: 0.002373060 seconds      Rate: 834 MB/s
    READ  => Size: 2097152 bytes      DMA: dma_map_*  Time: 0.002442500 seconds      Rate: 855 MB/s

You can run ``amba-pcitest -h`` to explore other available features and options.

Throughput Tunning
==================
* Check ``/sys/module/ambarella_cdns_udma/parameters/enable_mrrs_quirk``. This option is enabled by default on CV3 and CV5 because if a PC acts as an RC, EP UDMA INBOUND (IB) may receive corrupt data on some PCIe slots. You can disable this option if you don't have a PC as an RC or if your PC slot cannot reproduce this issue (you can use amba-pcitest to check). CV72 does not have this issue, so it is disabled by default on CV72.
* Increase the MRRS value. The maximum MRRS value can be up to 4096 bytes.

Debug Tips
==========
* If you suspect the issue is related to the DMA controller driver, enable the ``CONFIG_DMADEVICES_DEBUG`` and ``CONFIG_DMADEVICES_VDEBUG`` options.
* add ``-v`` option to ``amba-pcitest`` to output data.
* If you get error messages like "alloc failed", please add cma via dts:

  ::

   @@ -66,6 +66,12 @@ reserved-memory {
    		#address-cells = <2>;
    		#size-cells = <2>;
    		ranges;
   +		linux,cma {
   +			compatible = "shared-dma-pool";
   +			reusable;
   +			size = <0x0 0x08000000>;
   +			linux,cma-default;
   +		};
    	};

uDMA Throughput
==========
Please consult Ambarella support team for uDMA throughput.
