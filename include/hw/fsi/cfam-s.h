/*
 * IBM Common FRU Access Macro - S variant (CFAM-S)
 *
 * Copyright (C) 2026 IBM Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FSI_CFAM_S_H
#define FSI_CFAM_S_H

#include "system/memory.h"
#include "hw/fsi/fsi.h"
#include "hw/fsi/lbus.h"

#define CFAM_S_MBOX_SCRATCH_NUM 5

/*
 * Mailbox v1 engine of the CFAM-S. Like the scratchpad, it is a device on
 * the CFAM local bus; it exposes a small block of scratch registers.
 */
#define TYPE_FSI_CFAM_S_MBOX "cfam-s.mbox"
OBJECT_DECLARE_SIMPLE_TYPE(FSICFAMSMbox, FSI_CFAM_S_MBOX)

typedef struct FSICFAMSMbox {
    FSILBusDevice parent;

    uint32_t scratch[CFAM_S_MBOX_SCRATCH_NUM];
} FSICFAMSMbox;

#define TYPE_FSI_CFAM_S "cfam-s"
#define FSI_CFAM_S(obj) OBJECT_CHECK(FSICFAMSState, (obj), TYPE_FSI_CFAM_S)

typedef struct FSICFAMSState {
    /* < private > */
    FSISlaveState parent;

    /* CFAM-S exposes one 2 MiB register slot, aliased across four SID views */
    MemoryRegion mr;
    MemoryRegion slot;
    MemoryRegion slot_alias[3];
    MemoryRegion config_iomem;

    FSILBus lbus;
    FSICFAMSMbox mbox;
} FSICFAMSState;

#endif /* FSI_CFAM_S_H */
