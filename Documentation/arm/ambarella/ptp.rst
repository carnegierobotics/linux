.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella PTP
======================================================

Ambarella IEEE 1588 PTP clock
=============================

1. Overview
-----------
 IEEE 1588 Precision Clock Synchronization Protocol is supported on S5L, CV2, CV22, CV25
 and the later chips. `linuxptp` is a recommended tool to test and configure the ptp on linux.

2. PTP Configration and Test
----------------------------

 Linux Configration:

 ::

  CONFIG_PTP_1588_CLOCK=y

 Enter the following command as root on your HOST:

  ::

   $ ptp4l -i enp0s3 -m

 Slave only mode on your board side:

 ::

  # ptp4l -i eth0 -s -m

    root@#/home/default/ptp4l -i eth0 -s -m
    ptp4l[124.718]: selected /dev/ptp0 as PTP clock
    [  125.157591] SIOCSHWTSTAMP: f.0 t.1 f.c
    ptp4l[124.755]: port 1: INITIALIZING to LISTENING on INIT_COMPLETE
    ptp4l[124.755]: port 0: INITIALIZING to LISTENING on INIT_COMPLETE
    ptp4l[126.126]: port 1: new foreign master 8cec4b.fffe.958140-1
    ptp4l[130.127]: selected best master clock 8cec4b.fffe.958140
    ptp4l[130.127]: port 1: LISTENING to UNCALIBRATED on RS_SLAVE
    ptp4l[131.136]: master offset -1545626055960412635 s0 freq   -4819 path delay      9262
    ptp4l[132.136]: master offset -1545626055960417360 s1 freq   -9543 path delay      9262
    ptp4l[133.136]: master offset       4850 s2 freq   -4693 path delay      9262
    ptp4l[133.145]: port 1: UNCALIBRATED to SLAVE on MASTER_CLOCK_SELECTED
    ptp4l[134.137]: master offset       6450 s2 freq   -1638 path delay      7637
    ptp4l[135.137]: master offset       3350 s2 freq   -2803 path delay      7637
    ptp4l[136.137]: master offset       1300 s2 freq   -3848 path delay      7687
    ptp4l[137.138]: master offset        325 s2 freq   -4433 path delay      7737
    ptp4l[138.138]: master offset        100 s2 freq   -4560 path delay      7687
    ptp4l[139.138]: master offset       -175 s2 freq   -4805 path delay      7687
    ptp4l[140.138]: master offset        -25 s2 freq   -4708 path delay      7637
    ptp4l[141.139]: master offset         31 s2 freq   -4659 path delay      7631
    ptp4l[142.139]: master offset       -187 s2 freq   -4868 path delay      7649
    ptp4l[143.139]: master offset        -62 s2 freq   -4799 path delay      7649
    ptp4l[144.139]: master offset          6 s2 freq   -4750 path delay      7631

.. The Linux PTP Project website:
	https://linuxptp.sourceforge.net
.. Test and configure ptp use ptp4l:
	https://access.redhat.com/documentation/en-us/red_hat_enterprise_linux/6/html/deployment_guide/ch-configuring_ptp_using_ptp4l#s2-Advantages_of_PTP

3. PPS Configration and Test
----------------------------
3.1 make sure dts configure pps output and pps avail, here is an example on cv25.

::

		mac0: ethernet@e000e000 {
			compatible = "ambarella,eth";
			#address-cells = <1>;
			#size-cells = <0>;
			reg = <0xe000e000 0x2000>;
			interrupts = <0 58 0x4>;
			pinctrl-names = "default","pps"; ---------pinctrl name
			pinctrl-0 = <&rgmii_pins>;
			pinctrl-1 = <&enet_ptp_pps_o_pins_a>; ---------pinctrl configure
			phy-mode = "rgmii";
			amb,rct-regmap = <&rct_syscon>;
			amb,scr-regmap = <&scratchpad_syscon>;
			amb,tx-ring-size = <32>;
			amb,rx-ring-size = <64>;
			amb,ipc-tx;
			amb,ipc-rx;
			amb,enhance;
			amb,ahb-12mhz-div = <5>;
			clock-names = "ptp_ref";
			clocks= <&osc>;
			amb,ext-ref-clk;
			amb,pps-avail;		----------tell driver pps avail
			/* amb,mdio-gpio; */
			/* amb,tx-clk-invert; */
		};

3.2 use ioctl to configure pps period and enable output, code like this

::

	if (clock_gettime(clkid, &ts)) {
		perror("clock_gettime");
		return -1;
	}
	memset(&perout_request, 0, sizeof(perout_request));
	perout_request.index = index;
	perout_request.period.sec = perout / NSEC_PER_SEC;
	perout_request.period.nsec = perout % NSEC_PER_SEC;
	perout_request.flags = 0;

	perout_request.start.sec = ts.tv_sec + 2;
	perout_request.start.nsec = 0;

	if (ioctl(fd, PTP_PEROUT_REQUEST2, &perout_request)) {
		perror("PTP_PEROUT_REQUEST");
	} else {
		puts("periodic output request okay");
	}

3.3 log show and pps signal check

3.3.1 configure pps period and output enable

::

 # ./ppsenable -p 1000000000 		(ns)

3.3.2 on your board

::

 # ./ptp4l -i eth0 -s -m -H

3.3.3 on your pc

::

  # ptp4l -i eno1 -m -H
  ptp4l[99830.403]: selected /dev/ptp0 as PTP clock
  ptp4l[99830.404]: port 1: INITIALIZING to LISTENING on INITIALIZE
  ptp4l[99830.404]: port 0: INITIALIZING to LISTENING on INITIALIZE
  ptp4l[99830.404]: port 1: link up
  ptp4l[99837.481]: port 1: LISTENING to MASTER on ANNOUNCE_RECEIPT_TIMEOUT_EXPIRES
  ptp4l[99837.481]: selected best master clock d4bed9.fffe.a3c7ff
  ptp4l[99837.481]: assuming the grand master role

3.3.4 check pin on pps pin. (for example R14 on cv25_hazelnut, 1s square wave very precise)

3.3.5 total log on board

::

  # ls /dev/ptp0
  /dev/ptp0
  # ls
  # tftp -g 10.0.0.9 -r ppsenable
  ppsenable            100% |********************************| 4223k  0:00:00 ETA
  # tftp -g 10.0.0.9 -r ptp4l
  ptp4l                100% |********************************| 4897k  0:00:00 ETA
  # chmod +x ptp4l
  # chmod +x ppsenable
  # ./ppsenable -p 1000000000
  periodic output request okay
  #
  #
  # ./ptp4l -i eth0 -s -m -H
  ptp4l[88.714]: selected /dev/ptp0 as PTP clock
  [   89.117922] SIOCSHWTSTAMP: f.0 t.1 f.c
  ptp4l[88.752]: port 1: INITIALIZING to LISTENING on INIT_COMPLETE
  ptp4l[88.752]: port 0: INITIALIZING to LISTENING on INIT_COMPLETE
  ptp4l[93.144]: port 1: new foreign master d4bed9.fffe.a3c7ff-1
  [   96.780946] SIOCSHWTSTAMP: f.0 t.1 f.c
  ptp4l[96.415]: selected local clock 12a607.fffe.3312f4 as best master
  ptp4l[97.144]: selected best master clock d4bed9.fffe.a3c7ff
  ptp4l[97.145]: running in a temporal vortex
  ptp4l[97.145]: port 1: LISTENING to UNCALIBRATED on RS_SLAVE
  ptp4l[99.144]: master offset -10934776703318908 s0 freq      +0 path delay   1953245
  ptp4l[100.161]: master offset -10934776706997054 s1 freq -3692627 path delay   1623222
  ptp4l[101.152]: master offset    -330467 s2 freq -4023094 path delay   1623222
  ptp4l[101.152]: port 1: UNCALIBRATED to SLAVE on MASTER_CLOCK_SELECTED
  ptp4l[102.153]: master offset    -331364 s2 freq -4123132 path delay   1623222
  ptp4l[103.153]: master offset      97501 s2 freq -3793676 path delay   1293200
  ptp4l[104.153]: master offset     474696 s2 freq -3387230 path delay    686719
  ptp4l[105.153]: master offset    -159501 s2 freq -3879019 path delay    686719
  ptp4l[106.153]: master offset     302600 s2 freq -3464768 path delay     80238
  ptp4l[107.153]: master offset    -254303 s2 freq -3930891 path delay     80238
  ptp4l[108.153]: master offset    -330725 s2 freq -4083604 path delay     63969
  ptp4l[109.153]: master offset    -255119 s2 freq -4107215 path delay     47700
  ptp4l[110.153]: master offset    -162286 s2 freq -4090918 path delay     37802
  ptp4l[111.153]: master offset     -95528 s2 freq -4072846 path delay     37802
  ptp4l[112.153]: master offset     -36930 s2 freq -4042906 path delay     27905
  ptp4l[113.153]: master offset      -4158 s2 freq -4021213 path delay     13963
  ptp4l[114.153]: master offset       6182 s2 freq -4012121 path delay       893
  ptp4l[115.153]: master offset      -7359 s2 freq -4023807 path delay      2701
  ptp4l[116.153]: master offset      -6300 s2 freq -4024956 path delay      1464
  ptp4l[117.154]: master offset      -5284 s2 freq -4025830 path delay      1464
  ptp4l[118.153]: master offset      -2716 s2 freq -4024847 path delay       794
  ptp4l[119.153]: master offset      -1979 s2 freq -4024925 path delay       958
  ptp4l[120.154]: master offset      -1038 s2 freq -4024577 path delay       958
  ptp4l[121.154]: master offset       -526 s2 freq -4024377 path delay      1079
  ptp4l[122.154]: master offset        -29 s2 freq -4024038 path delay      1079
  ptp4l[123.153]: master offset         94 s2 freq -4023923 path delay      1016
  ptp4l[124.154]: master offset        139 s2 freq -4023850 path delay      1016
  ptp4l[125.154]: master offset        130 s2 freq -4023817 path delay      1016
  ptp4l[126.154]: master offset         -4 s2 freq -4023912 path delay      1016
  ptp4l[127.154]: master offset        -18 s2 freq -4023928 path delay      1016
  ptp4l[128.154]: master offset        -85 s2 freq -4024000 path delay      1092
  ptp4l[129.154]: master offset        -17 s2 freq -4023958 path delay      1092
  ptp4l[130.154]: master offset         47 s2 freq -4023899 path delay      1092
  ptp4l[131.154]: master offset         12 s2 freq -4023920 path delay      1095
  ptp4l[132.154]: master offset         32 s2 freq -4023896 path delay      1095
  ptp4l[133.154]: master offset        -47 s2 freq -4023965 path delay      1092
  ptp4l[134.154]: master offset         60 s2 freq -4023872 path delay      1083
  ptp4l[135.154]: master offset         29 s2 freq -4023885 path delay      1083
  ptp4l[136.154]: master offset        -20 s2 freq -4023926 path delay      1083
  ptp4l[137.154]: master offset         -5 s2 freq -4023917 path delay      1078
  ptp4l[138.154]: master offset         14 s2 freq -4023899 path delay      1083
  ptp4l[139.154]: master offset        -10 s2 freq -4023919 path delay      1083
  ptp4l[140.154]: master offset         15 s2 freq -4023897 path delay      1083
  ptp4l[141.154]: master offset        -41 s2 freq -4023949 path delay      1083
  ptp4l[142.154]: master offset         43 s2 freq -4023877 path delay      1074
  ptp4l[143.154]: master offset         19 s2 freq -4023888 path delay      1074
  ptp4l[144.154]: master offset         -6 s2 freq -4023907 path delay      1078
  ptp4l[145.154]: master offset        -36 s2 freq -4023939 path delay      1078
  ptp4l[146.154]: master offset         47 s2 freq -4023867 path delay      1078
  ptp4l[147.154]: master offset        -24 s2 freq -4023924 path delay      1078
  ptp4l[148.154]: master offset         27 s2 freq -4023880 path delay      1078
  ptp4l[149.155]: master offset         -2 s2 freq -4023901 path delay      1078
  ptp4l[150.155]: master offset         93 s2 freq -4023806 path delay      1078
  ptp4l[151.155]: master offset        -60 s2 freq -4023932 path delay      1079
