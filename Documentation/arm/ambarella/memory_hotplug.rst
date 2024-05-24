.. SPDX-License-Identifier: GPL-2.0

======================================================
Ambarella memory hotplug
======================================================

Kernel Configuration
====================

To use memory hotplug feature, kernel must be compiled with following
config options.

- For all memory hotplug:
    - Memory model -> Sparse Memory  (CONFIG_SPARSEMEM)
    - Allow for memory hot-add       (CONFIG_MEMORY_HOTPLUG)
	- Section size bits for sparsemem usage	 (CONFIG_SPARSEMEM_SECTION_SIZE_BITS)
	  It is used for configure the block_size_bytes when hotplug memory.
	  We set default value as 28. (1<<28 = 256MB)


sysfs files for memory hotplug
==============================

All memory blocks have their device information in sysfs.  Each memory block
is described under /sys/devices/system/memory as:

	/sys/devices/system/memory/memoryXXX
	(XXX is the memory block id.)

For the memory block covered by the sysfs directory.  It is expected that all
memory sections in this range are present and no memory holes exist in the
range. Currently there is no way to determine if there is a memory hole, but
the existence of one should not affect the hotplug capabilities of the memory
block.

For example, assume 1GiB memory block size. A device for a memory starting at
0x100000000 is /sys/device/system/memory/memory4::

	(0x100000000 / 1Gib = 4)

This device covers address range [0x100000000 ... 0x140000000)

Under each memory block, you can see 5 files:

- /sys/devices/system/memory/memoryXXX/phys_index
- /sys/devices/system/memory/memoryXXX/phys_device
- /sys/devices/system/memory/memoryXXX/state
- /sys/devices/system/memory/memoryXXX/removable
- /sys/devices/system/memory/memoryXXX/valid_zones

=================== ============================================================ ===========
``phys_index``      read-only and contains memory block id, same as XXX.
``state``           read-write

                    - at read:  contains online/offline state of memory.
                    - at write: user can specify "online_kernel",

                    "online_movable", "online", "offline" command
                    which will be performed on all sections in the block.
``phys_device``     read-only: designed to show the name of physical memory
                    device.  This is not well implemented now.
``removable``       read-only: contains an integer value indicating
                    whether the memory block is removable or not
                    removable.  A value of 1 indicates that the memory
                    block is removable and a value of 0 indicates that
                    it is not removable. A memory block is removable only if
                    every section in the block is removable.
``valid_zones``     read-only: designed to show which zones this memory block
		    can be onlined to.

		    The first column shows it`s default zone.

		    "memory6/valid_zones: Normal Movable" shows this memoryblock
		    can be onlined to ZONE_NORMAL by default and to ZONE_MOVABLE
		    by online_movable.

		    "memory7/valid_zones: Movable Normal" shows this memoryblock
		    can be onlined to ZONE_MOVABLE by default and to ZONE_NORMAL
		    by online_kernel.
=================== ============================================================ ===========
