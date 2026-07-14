ASPEED AST2700 UFS Host Controller
===================================

The AST2700 SoC includes a UFS host controller identified in the device tree
as ``aspeed,ufshc-m31-16nm``, mapped at ``0x12c08200`` (IRQ SPI 118). QEMU
models it as a sysbus frontend on top of the shared UFS core that also backs
the PCI UFS device (``hw/ufs/ufs.c`` and ``hw/ufs/lu.c``). The frontend only
provides the sysbus MMIO region, the interrupt line and the DMA address
space; all UFSHCI register behaviour, UTP transfer/task list processing,
UPIU and query handling and the SCSI logical-unit logic are implemented by
the core.

The clock/reset wrapper at ``0x12c08000`` (``aspeed,ast2700-ufscnr``) is left
as an ``UnimplementedDevice``.

Logical units
-------------

Storage is attached through ``ufs-lu`` devices on the controller's UFS bus,
exactly as for the PCI UFS device. On the ``huygens-bmc`` machine the board
automatically creates logical unit 0 from the first ``IF_NONE`` drive, with
a 512-byte logical block size to match the layout of the Huygens OpenBMC
image.

Usage
-----

Pass a UFS disk image with ``-drive if=none``; the board attaches it as
logical unit 0:

.. code-block:: console

  qemu-system-aarch64 -M huygens-bmc \
    -drive file=image-bmc,if=mtd,format=raw \
    -drive file=ufs.img,if=none,format=raw \
    -nographic

Please check :doc:`../../system/arm/aspeed` for more details on the
``huygens-bmc`` machine.
