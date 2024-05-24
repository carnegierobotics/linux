.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella Self-Refresh
======================================================

:Author: Rongrong <rrcao@ambarella.com>

0. Introduction
===============

0.1 The kernel supports up to four system sleep states generically:
-------------------------------------------------------------------

======= =============================================
mode    explain
======= =============================================
freeze  Suspend-To-Idle / ACPI S0
standby Standby / Power-On Suspend / ACPI S1
mem     Suspend-To-RAM / Self-Refresh / STR / ACPI S3
disk    Suspend-To-Disk / STD / ACPI S4
======= =============================================

For details, pelase see **Documentation/powr/states.txt**. Here we just
talk about “mem”, i.e., Self-Refresh (abbreviated as SR). In short
words, when system is in SR, all the HWs except for DRAM and PWC are
powered off.

0.2 SR and PWC (Power Control)
------------------------------

SR must co-work with PWC, and there are two options to use PWC:

1. internal PWC, but not all SoCs have internal PWC
2. external PWC (MCU) > for those SoCs without internal PWC, you have to
   use external MCU.

If the board design is to use external MCU for power sequence control,
the SoC needs to communicate with the MCU through a GPIO, and the
protocol is:

1. SoC generate a rising signal to notify mcu that soc will enter into
   SR
2. MCU notifies SoC the state after powered up:

   -  if cold boot, MCU keeps the GPIO high.
   -  if resume from SR, MCU keeps the GPIO low.

Different boards design may use different GPIO, and both BST and Kernel
need to know which GPIO is used to communicate with the external MCU.

-  For BST, we define the GPIO in .ini file by
   “PARAM_SOC_NOTIFY_MCU_GPIO”.
-  For Kernel, we define the GPIO in .dts file by
   “ambarella,gpio-notify-mcu”.

1. SelfRefresh
==============

System in the low power mode may cut down most of the power and thus
cause dram lose its content in this situation. The technology of DRAM
self-refresh can fixup this problem when system enter into the low power
mode and can quickly bring up the system.

2. Configuration
================

-  2.1 ATF

.. code:: bash

   make menuconfig

       Ambarella Firmware Configuration  --->
           AMBoot Options  --->
               [*] Boot with TrustZone  --->
           Memory Options  --->
               AMBoot memory options  --->
                   [0x100000] AMBoot boot loader starting address
                   [0x01400000] Firmware programmer starting address
               RTOS memory options  --->
                   [0x01280000] Kernel start address

-  2.2 Linux

.. code:: bash

   make menuconfig_public_linux

           Firmware Drivers  --->
               [*] Ambarella secure monitor driver
           Power management options  --->
              [*] Suspend to RAM and standby
              [*] User space wakeup sources interface
              (100) Maximum number of user space wakeup sources (0 = no limit)
              -*- Device power management core functionality

-  2.3 Ini ``InI`` file stay in your board’s bsp sub dir. Take
   s6lm_pineapple as an example
   ``ambarella/boards/s6lm_pineapple/bsp/xxx.ini``. Fill the
   communication line between soc and mcu

.. code:: bash

   CV22_walnut       :
       <PARAM_SOC_NOTIFY_MCU_GPIO value="142"/>
       <PARAM_MCU_NOTIFY_SOC_GPIO value="143"/>

   CV2_V100_chestnut :
       <PARAM_SOC_NOTIFY_MCU_GPIO value="146"/>
       <PARAM_MCU_NOTIFY_SOC_GPIO value="147"/>

   S6LM_pineapple:
       <PARAM_MCU_NOTIFY_SOC_GPIO value="81"/>
       <PARAM_SOC_NOTIFY_MCU_GPIO value="82"/>

   S5Lx_Tomato: <DRAM_RESET_CTRL_GPIO value="7"/>

-  2.4 DTS ``DTS`` file stay in your board’s bsp sub dir, Take
   s6lm_pineapple as an example
   ``ambarella/boards/s6lm_pineapple/bsp/s6lm_pineapple.dts``

.. code:: bash


   CV22_walnut: no change
   ---

   CV2_V100_chestnut: comment out gpio_keys
   #if 0
       gpio_keys {
           compatible = "gpio-keys";
           #address-cells = <1>;
           #size-cells = <0>;

           demo_button {
               label = "demo_button";
               status = "disabled";
               gpios = <&gpio 153 0>;
               debounce-interval = <10>;
               linux,code = <116>;
               gpio-key,wakeup;
           };
       };
   #endif
   ---

   S6LM_pineapple: no change
   ---

   S5Lx_Tomato:
   chosen {
       other content
       ambarella,pm-gpio-notify-mcu = <6>;
   }

--------------

3. Test
=======

1. Burn MCU code for CV2, CV22, S6LM
2. For CV22_walnut

.. code:: bash

       1. make cv22_dram_self_refresh_config
       2. make -j16            build first
       3. configure ATF.       ref 2.1
       4. configure Linux.     ref 2.2
       5. configure Ini.       ref 2.3
       6. configure dts.       ref 2.4
       7. burn firmware and boot system
       8. echo no > /sys/module/printk/parameters/console_suspend
       9. echo mem > /sys/power/state [you will see console become suspend]
       10. short-connect J65 PIN1<--->PIN6(like single click) will wakeup system
       11. console resume and test done

3. For CV2_V100_chestnut

.. code:: bash

       1. make cv2_dram_self_refresh_config
       2. make -j16              build first
       3. configure ATF.         ref 2.1
       4. configure Linux.       ref 2.2
       5. configure Ini.         ref 2.3
       6. configure dts.         ref 2.4
       7. burn firmware and boot system
       8. echo no > /sys/module/printk/parameters/console_suspend
       9. echo mem > /sys/power/state [you will see console become suspend]
       10. short-connect J65 PIN1<--->PIN6(like single click) will wakeup system
       11. console resume and test done

4. For S6LM_pineapple

.. code:: bash

       1. make s6lm_dram_self_refresh_config
       2. make -j16            build first
       3. configure ATF.         ref 2.1
       4. configure Linux.     ref 2.2
       5. configure Ini.       ref 2.3
       6. configure dts.       ref 2.4
       7. burn firmware and boot system
       8. echo no > /sys/module/printk/parameters/console_suspend
       9. echo mem > /sys/power/state [you will see console become suspend]
       10. short-connect J72 PIN1<--->PIN6(like single click) will wake up system
       11. console resume and test done

5. For S5Lx_Tomato AS before does

4. Not Apply SelfRefesh
=======================

For the system does not apply self-refresh, remove the configure In
``INI and LINUX. Ref2.2 and 2.3``;

5. Additional Information
=========================

-  5.1 DRAM_RESET_CTRL_GPIO This is used Only on S5Lx_Tomato board. To
   disconnect DRAM before enter into self-refresh mode.  

-  5.2 ambarella,pm-gpio-notify-mcu This signal come from soc and want
   to tell mcu that the system will be in low power mode soon and mcu
   will power down soc into self-refresh mode. The DRAM power must keep
   during self-refresh.  

-  5.3 PARAM_SOC_NOTIFY_MCU_GPIO Same as
   ``ambarella,pm-gpio-notify-mcu``, used on CV2/CV22/S6LM.  

-  5.4 PARAM_MCU_NOTIFY_SOC_GPIO This signal comes from mcu to tell soc
   next boot mode. ``PARAM_MCU_NOTIFY_SOC_GPIO=1`` soc will do normal
   boot else ``PARAM_MCU_NOTIFY_SOC_GPIO=0`` soc will do resume boot

-  5.5 ATF [ATF] arm-trusted-firmware: https://github.com/ARM-software/arm-trusted-firmware

6. Boot flow
============

::

                       ---------
                      |   BST   |
                       ---------
                           |
               ----------------------------
              | DRAM_RESET_CTRL_GPIO == -1 |    Y
              | Y `normal boot`            | -----------------------------------^
              | N `check self mode`        |                                    |
               ----------------------------                                     |
                           | N                                                  |
                           |                                                    |
                 -----------------------              --------------------      |
                | MCU_NOTIFY_GPIO == -1 |     Y      | PWC status[2]      |  0  |
                | Y `check PWC status`  |  ------>   | 1 `in self refresh`| ----+
                | N `check GPIO signal` |            | 0 `normal boot`    |     |
                 -----------------------             ---------------------      |
                           | N                                 | 1              |
                           |                                   |                |
               --------------------------                      |                |
              | MCU_NOTIFY_GPIO's signal |   high              |                |
              | high `normal boot`       | --------------------*----------------+
              | low  `in self refresh`   |                     |                |
               --------------------------                      |                |
                           | low                               |                |
                           |                                   |                |
               ---------------------------                     |                |
              | In self refresh mode.     |                    |                |
              | ATF text is in the DRAM   | < -----------------                 |
              | Jumping to ATF.           |                                     |
               ---------------------------                                      |
                           |                                                    |
                           |                                                    |
                -------------------------------                                 |
               | ATF                           |                 --------------------------------
               | a. `normal boot`              | <------------- | reload the ATF & BLD from NAND |
               | b. `resume from self refresh` |                 --------------------------------
                -------------------------------
                     a //            \\ b
                      //              \\
                     //                \\
                    //                  \\
                ---------            ---------
               | AMBoot  | -------> | LINUX   |
                ---------            ---------

..

   MCU_NOTIFY_GPIO means PARAM_MCU_NOTIFY_SOC_GPIO DRAM_RESET_CTRL_GPIO
   only for S5Lx_Tomato

--------------

[Boot flow Additional Supplement]

-  soc with PROGRAMMABLE_RESET_ADDRESS=1(core0 always boot from
   0xffff0000) core0->bst->bl31_cold_entry—>check_normal_or_resume_boot
   -> if normal boot then do normal boot as usual -> if resume boot then
   must jump to bl31_warm_entry to do resume boot  

-  soc with PROGRAMMABLE_RESET_ADDRESS=0(core0 always boot from
   0xffff0000) core1 cor2 core3 boot from 0x0000’0000 and use mail-box
   to check boot status

-  Enter into self-refresh, core1 core2 core3 shutdown first and stay in
   reset mode and core0 keeps working until soc power down.
