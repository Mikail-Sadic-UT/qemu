/*
 * IBM Common FRU Access Macro - S variant (CFAM-S)
 *
 * The CFAM-S is a derivative of the CFAM. Like the CFAM it presents a config
 * table describing the engines it contains and connects those engines to a
 * local bus (see cfam.c). The CFAM-S supports a limited set of engines: an
 * FSI responder and a v1 mailbox providing scratch registers.
 *
 * Copyright (C) 2026 IBM Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/units.h"
#include "trace.h"
#include "hw/fsi/cfam-s.h"
#include "hw/fsi/fsi.h"

/* One 2 MiB register slot, aliased into each of the four SID views (8 MiB) */
#define CFAM_SLOT_SIZE      (2 * MiB)
#define CFAM_WINDOW_SIZE    (8 * MiB)

/* Slot layout: 0x000 config table, 0x400 responder, 0x800 mailbox */
#define CFAM_RESPONDER_BASE 0x400 /* == FSI_RESPONDER_PAGE_SIZE */
#define CFAM_MBOX_BASE      0x800
#define CFAM_MBOX_SCRATCH_OFF 0xe0

/* Config-table word fields (see Linux fsi-master.h) */
#define CFAM_CONF_NEXT          (1u << 31)
#define CFAM_CONF_SLOTS(n)      (((n) & 0xff) << 16)
#define CFAM_CONF_VERSION(v)    (((v) & 0xf) << 12)
#define CFAM_CONF_TYPE(t)       (((t) & 0xff) << 4)
#define CFAM_CHIP_ID_MAJOR(m)   (((m) & 0xf) << 8)

/* Engine IDs (include/linux/fsi.h) */
#define FSI_ENGINE_ID_RESPONDER     0x3
#define FSI_ENGINE_ID_MBOXV1        0x14
#define FSI_CHIP_ID_MAJOR_CFAM_S    0x9 /* major == 9 -> CFAM-S */

static uint8_t cfam_s_crc4(uint8_t c, uint64_t x, int bits)
{
    static const uint8_t tab[16] = {
        0x0, 0x7, 0xe, 0x9, 0xb, 0xc, 0x5, 0x2,
        0x1, 0x6, 0xf, 0x8, 0xa, 0xd, 0x4, 0x3,
    };
    int i;

    x &= (1ull << bits) - 1;
    bits = (bits + 3) & ~0x3;
    for (i = bits - 4; i >= 0; i -= 4) {
        c = tab[c ^ ((x >> i) & 0xf)];
    }
    return c;
}

static uint32_t cfam_s_cfg_word(uint32_t fields)
{
    return fields | cfam_s_crc4(0, fields >> 4, 28);
}

/* Mailbox v1 engine: a local-bus device exposing scratch registers */
static uint64_t cfam_s_mbox_read(void *opaque, hwaddr addr, unsigned size)
{
    FSICFAMSMbox *mbox = FSI_CFAM_S_MBOX(opaque);

    trace_fsi_cfam_config_read(addr, size);

    if (addr >= CFAM_MBOX_SCRATCH_OFF &&
        addr < CFAM_MBOX_SCRATCH_OFF + CFAM_S_MBOX_SCRATCH_NUM * 4) {
        return mbox->scratch[(addr - CFAM_MBOX_SCRATCH_OFF) / 4];
    }
    return 0;
}

static void cfam_s_mbox_write(void *opaque, hwaddr addr, uint64_t data,
                              unsigned size)
{
    FSICFAMSMbox *mbox = FSI_CFAM_S_MBOX(opaque);

    trace_fsi_cfam_config_write(addr, size, data);

    if (addr >= CFAM_MBOX_SCRATCH_OFF &&
        addr < CFAM_MBOX_SCRATCH_OFF + CFAM_S_MBOX_SCRATCH_NUM * 4) {
        mbox->scratch[(addr - CFAM_MBOX_SCRATCH_OFF) / 4] = (uint32_t)data;
    }
}

static const MemoryRegionOps cfam_s_mbox_ops = {
    .read = cfam_s_mbox_read,
    .write = cfam_s_mbox_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void cfam_s_mbox_realize(DeviceState *dev, Error **errp)
{
    FSILBusDevice *ldev = FSI_LBUS_DEVICE(dev);

    memory_region_init_io(&ldev->iomem, OBJECT(ldev), &cfam_s_mbox_ops, ldev,
                          TYPE_FSI_CFAM_S_MBOX, 0x400);
}

static void cfam_s_mbox_reset(DeviceState *dev)
{
    FSICFAMSMbox *mbox = FSI_CFAM_S_MBOX(dev);

    memset(mbox->scratch, 0, sizeof(mbox->scratch));
}

static void cfam_s_mbox_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_FSI_LBUS;
    dc->realize = cfam_s_mbox_realize;
    device_class_set_legacy_reset(dc, cfam_s_mbox_reset);
}

static const TypeInfo cfam_s_mbox_info = {
    .name = TYPE_FSI_CFAM_S_MBOX,
    .parent = TYPE_FSI_LBUS_DEVICE,
    .instance_size = sizeof(FSICFAMSMbox),
    .class_init = cfam_s_mbox_class_init,
};

/* Config table: enumerates the engines present in the CFAM-S */
static uint64_t cfam_s_config_read(void *opaque, hwaddr addr, unsigned size)
{
    trace_fsi_cfam_config_read(addr, size);

    switch (addr) {
    case 0x00:
        /* chip-id: NEXT set, MAJOR=9 (CFAM-S) */
        return cfam_s_cfg_word(CFAM_CONF_NEXT |
                               CFAM_CHIP_ID_MAJOR(FSI_CHIP_ID_MAJOR_CFAM_S));
    case 0x04:
        /* responder engine entry */
        return cfam_s_cfg_word(CFAM_CONF_NEXT | CFAM_CONF_SLOTS(1) |
                               CFAM_CONF_VERSION(1) |
                               CFAM_CONF_TYPE(FSI_ENGINE_ID_RESPONDER));
    case 0x08:
        /* mailbox engine entry, last (NEXT clear) */
        return cfam_s_cfg_word(CFAM_CONF_SLOTS(1) | CFAM_CONF_VERSION(1) |
                               CFAM_CONF_TYPE(FSI_ENGINE_ID_MBOXV1));
    default:
        return 0;
    }
}

static void cfam_s_config_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
    trace_fsi_cfam_config_write(addr, size, data);
}

static const MemoryRegionOps cfam_s_config_ops = {
    .read = cfam_s_config_read,
    .write = cfam_s_config_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

/* Backdrop for slot offsets not covered by the config table or an engine */
static uint64_t cfam_s_unimpl_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void cfam_s_unimpl_write(void *opaque, hwaddr addr, uint64_t data,
                                unsigned size)
{
}

static const MemoryRegionOps cfam_s_unimpl_ops = {
    .read = cfam_s_unimpl_read,
    .write = cfam_s_unimpl_write,
    .endianness = DEVICE_BIG_ENDIAN,
};

static void fsi_cfam_s_instance_init(Object *obj)
{
    FSICFAMSState *cfam = FSI_CFAM_S(obj);

    object_initialize_child(obj, "mbox", &cfam->mbox, TYPE_FSI_CFAM_S_MBOX);
}

static void fsi_cfam_s_realize(DeviceState *dev, Error **errp)
{
    FSICFAMSState *cfam = FSI_CFAM_S(dev);

    /* Build the single 2 MiB register slot */
    memory_region_init_io(&cfam->slot, OBJECT(cfam), &cfam_s_unimpl_ops, cfam,
                          TYPE_FSI_CFAM_S ".slot", CFAM_SLOT_SIZE);

    memory_region_init_io(&cfam->config_iomem, OBJECT(cfam), &cfam_s_config_ops,
                          cfam, TYPE_FSI_CFAM_S ".config", CFAM_RESPONDER_BASE);
    memory_region_add_subregion(&cfam->slot, 0, &cfam->config_iomem);

    /*
     * Responder engine: the FSI slave control-register block inherited from
     * TYPE_FSI_SLAVE (the same block cfam.c maps for the regular CFAM).
     */
    memory_region_add_subregion(&cfam->slot, CFAM_RESPONDER_BASE,
                                &FSI_SLAVE(cfam)->iomem);

    qbus_init(&cfam->lbus, sizeof(cfam->lbus), TYPE_FSI_LBUS, DEVICE(cfam),
              NULL);

    if (!qdev_realize(DEVICE(&cfam->mbox), BUS(&cfam->lbus), errp)) {
        return;
    }
    memory_region_add_subregion(&cfam->lbus.mr, 0,
                                &FSI_LBUS_DEVICE(&cfam->mbox)->iomem);
    memory_region_add_subregion(&cfam->slot, CFAM_MBOX_BASE, &cfam->lbus.mr);

    /*
     * The slot is visible in each of the four SID views. Alias it across the
     * 8 MiB window so both the SID_BREAK enumeration view and the runtime view
     * reach the same registers.
     */
    memory_region_init(&cfam->mr, OBJECT(cfam), TYPE_FSI_CFAM_S,
                       CFAM_WINDOW_SIZE);
    memory_region_add_subregion(&cfam->mr, 0, &cfam->slot);
    for (int i = 0; i < 3; i++) {
        memory_region_init_alias(&cfam->slot_alias[i], OBJECT(cfam),
                                 TYPE_FSI_CFAM_S ".slot-alias", &cfam->slot, 0,
                                 CFAM_SLOT_SIZE);
        memory_region_add_subregion(&cfam->mr, (i + 1) * CFAM_SLOT_SIZE,
                                    &cfam->slot_alias[i]);
    }
}

static void fsi_cfam_s_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_FSI_BUS;
    dc->realize = fsi_cfam_s_realize;
}

static const TypeInfo fsi_cfam_s_info = {
    .name = TYPE_FSI_CFAM_S,
    .parent = TYPE_FSI_SLAVE,
    .instance_size = sizeof(FSICFAMSState),
    .instance_init = fsi_cfam_s_instance_init,
    .class_init = fsi_cfam_s_class_init,
};

static void fsi_cfam_s_register_types(void)
{
    type_register_static(&cfam_s_mbox_info);
    type_register_static(&fsi_cfam_s_info);
}

type_init(fsi_cfam_s_register_types);
