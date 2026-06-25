/*
 * IBM Common FRU Access Macro (CFAM)
 *
 * Copyright (C) 2024 IBM Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef FSI_CFAM_H
#define FSI_CFAM_H

#include "system/memory.h"
#include "hw/fsi/fsi.h"

#define TYPE_FSI_CFAM "cfam"
#define FSI_CFAM(obj) OBJECT_CHECK(FSICFAMState, (obj), TYPE_FSI_CFAM)

#define CFAM_MBOX_SCRATCH_NUM 5

typedef struct FSICFAMState {
    FSISlaveState parent;

    MemoryRegion mr;
    uint32_t mbox_scratch[CFAM_MBOX_SCRATCH_NUM];
} FSICFAMState;

#endif /* FSI_CFAM_H */
