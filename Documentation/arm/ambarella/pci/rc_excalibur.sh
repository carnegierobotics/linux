#!/bin/sh

num=$1

if [ -z "$num" ]; then
    num=1000000
fi

for i in $(seq 1 "$num");
  do modprobe excalibur_RC_rc_ob && rmmod excalibur_RC_rc_ob && modprobe excalibur_RC_rc_ib && rmmod excalibur_RC_rc_ib && modprobe excalibur_RC_ep_ob && rmmod excalibur_RC_ep_ob && modprobe excalibur_RC_ep_ib && rmmod excalibur_RC_ep_ib;
done
