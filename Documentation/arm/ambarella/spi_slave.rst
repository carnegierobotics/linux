.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella SPI Slave
======================================================

:Author: Rongrong <rrcao@ambarella.com>

Introduction
============

Serial Peripheral Interface(SPI) is a synchronized communication interface used in
embedded systems. The embedded systems communicating via SPI are in a master-slave
relationship. The master is the controlling device and the slave takes instructions
from the master. The simplest configuration of the SPI is a single master, single
slave system; but one master can control more than one slave.

Ambarella SoCs are equipped with a single SPI Slave controller, and the speed on
SPI slave is designed to be up to 8MHz. But, when the SPI signal is used on board
or even across boards, the real reliable speed should be fine-tuned based on real
system loading conditions.



Abbreviations
=============

SCLK: the clock driven by the master device.
MOSI: the line for the data from master to slave(Master Output Slave Input)
MISO: the line for the data from slave to master(Master Input Slave Output)
SS:   the slave select and sometimes, it's also known as CS(Chip Select)



Driver Configuration
====================

Device Drivers  --->
         [*] SPI support  --->
                  <M>   Ambarella SPI Slave Controller

The module should be present in "drivers/spi/spi-slave-ambarella.ko".



Device Tree
===========

Please see Documentation/devicetree/bindings/arm/ambarella.txt - 18) - SLAVE SPI node



Usage
=====

On loading the module(spi-slave-ambarella.ko) you can see /dev/slavespi is created.

/dev/slavespi is a "miscdev" device, however it can support below standard SPI IOCTLs
which are defined by "spidev":
SPI_IOC_RD_MODE
SPI_IOC_RD_BITS_PER_WORD
SPI_IOC_RD_MAX_SPEED_HZ
SPI_IOC_WR_MODE
SPI_IOC_WR_BITS_PER_WORD
SPI_IOC_WR_MAX_SPEED_HZ
SPI_IOC_MESSAGE(1)

Please refer to Documentation/spi/spidev.rst for details.

Besides the unsupported IOCTLs, the other differences of usage between "slavespi" and
"spidev" are described as below:

a) "slavespi" can only use ioctl() to request a SPI transfer, but cannot support read()
   and write().

b) "slavespi" can only support SPI_IOC_MESSAGE(1), while "spidev" can support
   SPI_IOC_MESSAGE(n), here n can be greater than 1.

c) When using SPI_IOC_MESSAGE(1) to request a SPI transfer, a struct spi_ioc_transfer
   will be provided as argument of SPI_IOC_MESSAGE(1). However, "slavespi" will only
   use and/or check these fields:
   tx_buf, rx_buf, len, speed_hz, delay_usecs and bits_per_word.
   In addition, delay_usecs has different meaning between "slavespi" and "spidev",
   please see d) below for details.

d) For "spidev", delay_usecs in struct spi_ioc_transfer means how long to delay after
   the last bit transfer before optionally deselecting the device before the next
   transfer. This field is useless for "slavespi" as it plays in passive role. So that
   "slavespi" will take delay_usecs for another usage, that is, the SPI_IOC_MESSAGE(1)
   will return after delay_usecs milliseconds even though the SPI transfer isn't done
   yet. This is useful for application in user space sometimes, as "slavespi" is in
   passive role, it can only prepare the transfer, and then wait for master to start
   the SPI transfer. In case the SPI master never start the transfer, the application
   in user space can never get back from SPI_IOC_MESSAGE(1).

   PS: the unit of delay_usecs is milliseconds rather than microseconds, its max value
   is 65536, and if delay_usecs is zero, SPI_IOC_MESSAGE(1) will wait forever until
   the transfer is done, unless a "interupt" signal(SIGINT) is received from user
   space.



Application
===========

Please refer to unit_test/linux/misc/test_spi.c



NOTE
====

1) Ambarella Slave SPI controller can only support Motorola SPI, and currently only
   mode_1(0|SPI_CPHA) and mode_3(SPI_CPOL|SPI_CPHA) are supported.
   In addition, bit-per-words can only be 8 or 16.

2) By default, the max transfer size of each SPI transfer is 2K bytes, it's determined
   by the dma buffer size. If you want to transfer more size than 2KB per transfer,
   please set "amb,dma-buf-size" in devicetree properly.
   Pleaser refer to Documentation/devicetree/bindings/arm/ambarella.txt.

3) SPI slave is in passive role, and the SPI master might assert SS at any given time
   and toggle clocks directly to drive or receive data from the SPI slave, so there is
   possibility that the SPI master starts transfer when the SPI slave is not ready, in
   such case, data corruption might be observed.

   In order to avoid above case, it's strongly recommend to add a slave ready signal in
   addition to the four standard SPI signals between SPI master and slave that act as
   a flow control signal and is driven by the SPI slave. This signal should be assered
   by the SPI slave only when it has already been ready for the transfer. Once the SPI
   master detect the assertion of Slave Ready, the SPI master can asser SS and start
   the transfer.
   PS: if there is such salve ready signal can be used, it can be added as "notify-gpio"
   into devicetree to let the "slavespi" driver know.
   Pleaser refer to Documentation/devicetree/bindings/arm/ambarella.txt.
   NOTE:
   It's REQUIRED for master to start the transfer when the edge of "notify-gpio" is
   detected. As for "rising" or "falling", it's determined by how "notify-gpio" is set.
   E.g.

         #. "rising":  notify-gpio = <&gpio 69 0x1>;
         #. "falling": notify-gpio = <&gpio 69 0x0>;

   If there is no such slave ready signal, a software protocol must be designed and
   implemented to protect the SPI slave.

4) Unlike UART that has some parity check and I2C that has flow control, SPI is designed
   to be simple and effective. In a real system, considering the possible board design,
   there could be signal interference or distortion on the SPI connection to induce
   possible incorrect signal and corupt the exchanged data.

   Therefore, a software protocol shall be designed and implemented to detect the data
   corruption and use a retransmission mechanism to increase the reliability of transfer.


