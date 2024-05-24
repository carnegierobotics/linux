#!/bin/sh

num=$1

if [ -z "$num" ]; then
    num=1000000
fi

for i in $(seq 1 "$num");
  do modprobe excalibur_EP_rc_ob && rmmod excalibur_EP_rc_ob && modprobe excalibur_EP_rc_ib && rmmod excalibur_EP_rc_ib && modprobe excalibur_EP_ep_ob && rmmod excalibur_EP_ep_ob && modprobe excalibur_EP_ep_ib && rmmod excalibur_EP_ep_ib;
done
