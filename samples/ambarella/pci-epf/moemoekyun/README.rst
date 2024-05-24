RC OB
=====

#. RC: transfer data: ``modprobe RC_rc_ob``
#. EP: check data ``modprobe EP_rc_ob``

RC IB
=====

#. EP: prepares data: ``modprobe EP_rc_ib``
#. RC: transfer and check data: ``modprobe RC_rc_ib``

EP OB
=====

#. EP: transfer data: ``modprobe EP_ep_ob``
#. RC: check data: ``modprobe RC_ep_ob``

EP IB
=====

#. RC: prepare data: ``modprobe RC_ep_ib``
#. EP: transfer and check data: ``modprobe EP_ep_ib``
