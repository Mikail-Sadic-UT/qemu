/*
 * IBM Huygens
 *
 * Copyright 2026 IBM Corp.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "hw/arm/machines-qom.h"
#include "hw/arm/aspeed.h"
#include "hw/arm/aspeed_soc.h"
#include "hw/nvram/eeprom_at24c.h"
#include "hw/i2c/i2c_mux_pca954x.h"
#include "hw/sensor/tmp105.h"

/* Currently based on AST2700 evb hardware value */
/* Controls Boot source, clk freq, pin multiplex, mem config, periphs, sercurity */

#define HUYGENS_BMC_HW_STRAP1 0x00000800    /* SCU HW Strap1 */
#define HUYGENS_BMC_HW_STRAP2 0x00000700    /* SCUIO HW Strap1 */

/* Huygens BMC FRU - placeholder data */
/* Stores inventory info abt machine */
static const uint8_t huygens_bmc_fruid[] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x84,
    0x28, 0x00, 0x52, 0x54, 0x04, 0x56, 0x48, 0x44, 0x52, 0x56, 0x44, 0x02,
    0x01, 0x00, 0x50, 0x54, 0x0e, 0x56, 0x54, 0x4f, 0x43, 0x00, 0x00, 0x37,
    0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x46, 0x08, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x1c, 0x00, 0x52, 0x54,
    0x04, 0x56, 0x54, 0x4f, 0x43, 0x50, 0x54, 0x0e, 0x56, 0x49, 0x4e, 0x49,
    0x00, 0x00, 0x57, 0x00, 0x1e, 0x00, 0x00, 0x00, 0x00, 0x00, 0x50, 0x46,
    0x01, 0x00, 0x00, 0x00, 0x1a, 0x00, 0x52, 0x54, 0x04, 0x56, 0x49, 0x4e,
    0x49, 0x44, 0x52, 0x04, 0x44, 0x45, 0x53, 0x43, 0x48, 0x57, 0x02, 0x30,
    0x31, 0x50, 0x46, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const size_t huygens_bmc_fruid_len = sizeof(huygens_bmc_fruid);

static void huygens_bmc_i2c_init(AspeedMachineState *bmc)
{
    AspeedSoCState *soc = bmc->soc;

    /* I2C0: BMC EEPROM */
    at24c_eeprom_init_rom(aspeed_i2c_get_bus(&soc->i2c, 0), 0x50,
                          8 * KiB, huygens_bmc_fruid, huygens_bmc_fruid_len);

    /* I2C2: TPM */

    /* I2C8: System VPD and LCD EEPROM with MCU mux */
    at24c_eeprom_init(aspeed_i2c_get_bus(&soc->i2c, 8), 0x53, 4 * KiB);  /* SYSVPD-primary */
    at24c_eeprom_init(aspeed_i2c_get_bus(&soc->i2c, 8), 0x51, 8 * KiB);  /* LCD */
    
    /* MCU I2C mux on bus 8 */
    // i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->1i2c, 8),
    //                         "pca9548", 0x70);

    /* I2C9: System VPD redundant and op-panel devices */
    at24c_eeprom_init(aspeed_i2c_get_bus(&soc->i2c, 9), 0x53, 8 * KiB);  /* SYSVPD-redundant */
    
    /* TMP275 temperature sensor (compatible with TMP105) */
    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 9), TYPE_TMP105, 0x48);
    
    /* Op-panel EEPROM */
    at24c_eeprom_init(aspeed_i2c_get_bus(&soc->i2c, 9), 0x51, 8 * KiB);
    
    /* PCA9552 LED controllers on bus 9 */
    aspeed_create_pca9552(soc, 9, 0x62);
    aspeed_create_pca9552(soc, 9, 0x64);
    aspeed_create_pca9552(soc, 9, 0x66);
    
    /* DPS310 pressure sensor */
    i2c_slave_create_simple(aspeed_i2c_get_bus(&soc->i2c, 9), "dps310", 0x76);
}

static void aspeed_machine_huygens_class_init(ObjectClass *oc,
                                                    const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);
    AspeedMachineClass *amc = ASPEED_MACHINE_CLASS(oc);

    mc->alias      = "ast2700-huygens";
    mc->desc       = "Aspeed AST2700 A2 [IBM Huygens]";
    amc->soc_name  = "ast2700-a2";
    amc->hw_strap1 = HUYGENS_BMC_HW_STRAP1;
    amc->hw_strap2 = HUYGENS_BMC_HW_STRAP2;
    amc->fmc_model = "w25q01jvq";
    amc->spi_model = "w25q512jv";
    amc->num_cs    = 2;
    amc->macs_mask = ASPEED_MAC0_ON | ASPEED_MAC1_ON | ASPEED_MAC2_ON;
    amc->uart_default = ASPEED_DEV_UART12;
    amc->i2c_init  = huygens_bmc_i2c_init;
    amc->vbootrom = true;
    mc->default_ram_size = 2 * GiB;
    aspeed_machine_class_init_cpus_defaults(mc);
}

static const TypeInfo aspeed_ast27x0_huygens_types[] = {
    {
        .name          = MACHINE_TYPE_NAME("huygens-bmc"),
        .parent        = TYPE_ASPEED_MACHINE,
        .class_init    = aspeed_machine_huygens_class_init,
        .interfaces    = aarch64_machine_interfaces,
    }
};

DEFINE_TYPES(aspeed_ast27x0_huygens_types)
