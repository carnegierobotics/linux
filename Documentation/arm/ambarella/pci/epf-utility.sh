#!/usr/bin/env sh
# SPDX-License-Identifier: GPL-2.0
# Author: Li Chen <lchen@ambarella.com>
# Copyright (c) 2023 Ambarella International LP

VENDOR_ID='0x17cd'
DMAENGINE="udma"
CONFIGSFS_PATH='/sys/kernel/config'

epf_init() {
    #set -x

    modprobe "$2"
    if ! mountpoint -q "$CONFIGSFS_PATH"; then
        mount -t configfs none "$CONFIGSFS_PATH"
    fi
    mkdir -p "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0
    if [ $? -ne 0 ] ; then
        echo "It seems that you forgot to enable $3, please enable it, and then try again"
        exit 1
    fi
    echo "$VENDOR_ID" > "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0/vendorid
    echo "$DEVICE_ID" > "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0/deviceid
    echo 16 > "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0/msi_interrupts
    echo 8 > "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0/msix_interrupts
    ln -s "$CONFIGSFS_PATH"/pci_ep/functions/"$3"/func0  "$CONFIGSFS_PATH"/pci_ep/controllers/"$1"
    if [ $? -ne 0 ] ; then
        exit 1
    fi
    echo 1 > "$CONFIGSFS_PATH"/pci_ep/controllers/"$1"/start
    #set +x
    echo "ep setup successfully and can startup RC, dont forget to add ${VENDOR_ID} and ${DEVICE_ID} to RC-side function driver, e.g. drivers/misc/pci_endpoint_test.c for pci_epf_test)"
}

epf_diag() {
    if dmesg | grep -F "$USER_EPC" | grep -q "device failed to get specific reserved mem pool"; then
        echo "ERROR: please assign a reserved mempool node to your targe pcie ep node, otherwise tests like irq will fail!"
        exit 1
    fi
    if ! dmesg | grep -q -F "$DMAENGINE: Register successfully"; then
        echo "WARNINGA: it seems you forget to enable your private dma engine"
        exit 1
    fi
}

epf_get_epc() {
        PCI_EPC='/sys/class/pci_epc/'
        NR_EPC=$(ls ${PCI_EPC} | wc -l)
        echo 'we have' "${NR_EPC}" 'endpoint function controller(s)'

        ls $PCI_EPC > epc.list
        if [ "$NR_EPC" -gt "1" ]; then
                echo 'Please select from the epc list:'
                nl epc.list
                count="$(wc -l epc.list | cut -f 1 -d' ')"
                while true; do
                    echo "please select"
                    printf "input an option: " >&2
                    read -r n
                    # If $n is an integer between one and $count...
                    if [ "$n" -eq "$n" ] && [ "$n" -gt 0 ] && [ "$n" -le "$count" ]; then

                        break
                    fi
                done
                USER_EPC="$(sed -n "${n}p" epc.list)"
        elif [ "$NR_EPC" -eq 0 ]; then
                echo "there is no epc available, please check your dts, kernel config, and driver"
                exit 1
        else
                USER_EPC="$(sed -n "1p" epc.list)"
        fi
}

epf_cleanup() {
        rm epc.list
}
