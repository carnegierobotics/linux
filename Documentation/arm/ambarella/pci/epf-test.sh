#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0
# Author: Li Chen <lchen@ambarella.com>
# Copyright (c) 2023 Ambarella International LP

. $(dirname "$0")/epf-utility.sh

DEVICE_ID='0x0200'

epf_get_epc
epf_diag
epf_init "$USER_EPC" "pci-epf-test" "pci_epf_test"
