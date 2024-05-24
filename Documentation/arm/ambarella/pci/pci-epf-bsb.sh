#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0
# Author: Li Chen <lchen@ambarella.com>
# Copyright (c) 2023 Ambarella International LP
# This script is for drivers/pci/endpoint/functions/ambarella_bsb_ep.c

. $(dirname "$0")/epf-utility.sh

DEVICE_ID='0x0307'

epf_get_epc
epf_diag
epf_init "$USER_EPC" "ambarella_bsb_ep" "pci_epf_bsb"
