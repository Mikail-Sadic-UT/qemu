/*
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Copyright (C) 2024 IBM Corp.
 *
 * IBM Common FRU Access Macro (CFAM)
 *
 * Reworked to model what the new Linux FSI "responder" framework expects
 * (drivers/fsi/{controller-*,responder}.c), rather than the old fsi-core
 * layout. The framework:
 *   - addresses a CFAM via the slave-id field in address bits [22:21]; it
 *     enumerates using SID_BREAK (id 3 -> 0x600000) and runs at SID 0;
 *   - reads a config table (CRC4-checked words) at the SID_BREAK address and
 *     requires a "responder" engine entry;
 *   - then accesses the FSI slave (responder) registers at engine_addr (0x400).
 *
 * We additionally model a CFAM-S mailbox engine so the kernel mbox driver
 * (drivers/fsi/fsi-mbox-cfam-s.c, engine id mboxv1=0x14) binds and creates
 * /dev/fsi/mbox0, which rbmc-cfamd requires. The mbox driver does read /
 * write / read-modify-write of scratchpad registers at engine offset
 * 0xe0 + reg*4, so those must hold state.
 *
 * We fold the SID bits away so the SID_BREAK and SID-0 views hit the same
 * registers.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "trace.h"

#include "hw/fsi/cfam.h"
#include "hw/fsi/fsi.h"

#include "hw/core/qdev-properties.h"

/*
 * Slave id in address bits [22:21]; per-slave space is 1 << 21 (2 MiB). Map
 * the whole 1 << 23 (8 MiB) so the SID_BREAK view (3 << 21 = 0x600000) is
 * covered, and fold the SID bits in the handlers.
 */
#define CFAM_SID_MASK            0x1fffff   /* low 21 bits within a slave */
#define CFAM_WINDOW_SIZE         0x800000   /* 8 MiB, covers SID 0..3 */

#define CFAM_RESPONDER_BASE      0x400      /* == FSI_RESPONDER_PAGE_SIZE */

/* Config-table word fields (see Linux responder-regs.h) */
#define CFAM_CONF_NEXT           (1u << 31)
#define CFAM_CONF_SLOTS(n)       (((n) & 0xff) << 16)
#define CFAM_CONF_VERSION(v)     (((v) & 0xf) << 12)
#define CFAM_CONF_TYPE(t)        (((t) & 0xff) << 4)
#define CFAM_CHIP_ID_MAJOR(m)    (((m) & 0xf) << 8)

/* engine ids (include/linux/fsi.h) */
#define FSI_ENGINE_ID_RESPONDER  0x3
#define FSI_ENGINE_ID_MBOXV1     0x14
#define FSI_CHIP_ID_MAJOR_CFAM_S 0x9        /* major == 9 -> CFAM-S */

/*
 * Engine layout (folded offsets), single CFAM-S on link 0:
 *   0x000  config table
 *   0x400  responder (FSI slave) registers  (config[1], 1 slot)
 *   0x800  mailbox engine                   (config[2], 1 slot)
 * The mbox driver reads/writes scratchpad regs at engine off 0xe0 + reg*4.
 */
#define CFAM_MBOX_BASE           0x800
#define CFAM_MBOX_SCRATCH_OFF    0xe0
#define CFAM_MBOX_SCRATCH_BASE   (CFAM_MBOX_BASE + CFAM_MBOX_SCRATCH_OFF)
#define CFAM_MBOX_SCRATCH_NUM    5          /* regs 0..4 (driver allows <= 4) */

/*
 * Mailbox scratchpad state. rbmc-cfamd does read-modify-write, so reads must
 * return prior writes. Only the link-0 CFAM-S is ever driven in this model,
 * so a single shared array is sufficient.
 */
static uint32_t cfam_mbox_scratch[CFAM_MBOX_SCRATCH_NUM];

/* Linux lib/crc4.c (poly 0b10111), table-driven, MSB-first nibbles. */
static uint8_t cfam_crc4(uint8_t c, uint64_t x, int bits)
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

/*
 * Build a config-table word with a valid CRC4 in the low nibble such that
 * crc4(0, word, 32) == 0. 'fields' must have its low nibble clear.
 */
static uint32_t cfam_cfg_word(uint32_t fields)
{
    return fields | cfam_crc4(0, fields >> 4, 28);
}

static uint64_t fsi_cfam_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t off = (uint32_t)addr & CFAM_SID_MASK;
    uint32_t val = 0;

    if (off < CFAM_RESPONDER_BASE) {
        /* CFAM configuration table (walked in 4-byte entries). */
        switch (off) {
        case 0x00:
            /* chip id: NEXT set, MAJOR=9 -> CFAM-S. */
            val = cfam_cfg_word(CFAM_CONF_NEXT |
                                CFAM_CHIP_ID_MAJOR(FSI_CHIP_ID_MAJOR_CFAM_S));
            break;
        case 0x04:
            /* responder engine, 1 slot, NEXT set (mbox follows). */
            val = cfam_cfg_word(CFAM_CONF_NEXT | CFAM_CONF_SLOTS(1) |
                                CFAM_CONF_VERSION(1) |
                                CFAM_CONF_TYPE(FSI_ENGINE_ID_RESPONDER));
            break;
        case 0x08:
            /* mailbox engine, 1 slot, last entry (NEXT clear). */
            val = cfam_cfg_word(CFAM_CONF_SLOTS(1) | CFAM_CONF_VERSION(1) |
                                CFAM_CONF_TYPE(FSI_ENGINE_ID_MBOXV1));
            break;
        default:
            val = 0;    /* NEXT already clear: the walk has stopped. */
            break;
        }
    } else if (off >= CFAM_MBOX_SCRATCH_BASE &&
               off < CFAM_MBOX_SCRATCH_BASE + CFAM_MBOX_SCRATCH_NUM * 4) {
        /* Mailbox scratchpad registers (stateful). */
        val = cfam_mbox_scratch[(off - CFAM_MBOX_SCRATCH_BASE) / 4];
    } else {
        /*
         * Responder (FSI slave) register block and everything else: safe
         * defaults are enough for enumeration/registration.
         *   - SSTAT = 0 -> LBUS owner == id == 0 (this BMC owns the bus)
         *   - SISS  = 0 -> no pending interrupt conditions
         */
        val = 0;
    }

    trace_fsi_cfam_config_read(addr, size);
    return val;
}

static void fsi_cfam_write(void *opaque, hwaddr addr, uint64_t data,
                           unsigned size)
{
    uint32_t off = (uint32_t)addr & CFAM_SID_MASK;

    if (off >= CFAM_MBOX_SCRATCH_BASE &&
        off < CFAM_MBOX_SCRATCH_BASE + CFAM_MBOX_SCRATCH_NUM * 4) {
        /* Mailbox scratchpad: store so read-modify-write works. */
        cfam_mbox_scratch[(off - CFAM_MBOX_SCRATCH_BASE) / 4] = (uint32_t)data;
    }
    /*
     * All other writes (config table, BREAK, SRES, SMODE, SSISM, SLBUS,
     * LLMODE, ...) are accepted no-ops: nothing needs state for the scan +
     * responder/mbox-registration path.
     */
    trace_fsi_cfam_config_write(addr, size, data);
}

static const MemoryRegionOps cfam_ops = {
    .read = fsi_cfam_read,
    .write = fsi_cfam_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 4,
    .impl.min_access_size = 1,
    .impl.max_access_size = 4,
};

static void fsi_cfam_realize(DeviceState *dev, Error **errp)
{
    FSICFAMState *cfam = FSI_CFAM(dev);

    memory_region_init_io(&cfam->mr, OBJECT(cfam), &cfam_ops, cfam,
                          TYPE_FSI_CFAM, CFAM_WINDOW_SIZE);
}

static void fsi_cfam_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->bus_type = TYPE_FSI_BUS;
    dc->realize = fsi_cfam_realize;
}

static const TypeInfo fsi_cfam_info = {
    .name = TYPE_FSI_CFAM,
    .parent = TYPE_FSI_SLAVE,
    .instance_size = sizeof(FSICFAMState),
    .class_init = fsi_cfam_class_init,
};

static void fsi_cfam_register_types(void)
{
    type_register_static(&fsi_cfam_info);
}

type_init(fsi_cfam_register_types);
