/*
 * Xilinx Zynq MPSoC emulation
 *
 * Copyright (C) 2015 Xilinx Inc
 * Written by Peter Crosthwaite <peter.crosthwaite@xilinx.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the
 * Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 * for more details.
 */

#include "hw/arm/xlnx-zynqmp.h"
#include "hw/intc/arm_gic_common.h"
#include "exec/address-spaces.h"
#include "sysemu/sysemu.h"
#include "qemu-plugin.h"
#include "gnat-bus.h"

#define GIC_NUM_SPI_INTR 160

#define ARM_PHYS_TIMER_PPI  30
#define ARM_VIRT_TIMER_PPI  27

#define GIC_BASE_ADDR       0xf9000000
#define GIC_DIST_ADDR       0xf9010000
#define GIC_CPU_ADDR        0xf9020000
#define CRF_APB_ADDR        0xfd1a0000
#define CRL_APB_ADDR        0xff5e0000

static const uint64_t gem_addr[XLNX_ZYNQMP_NUM_GEMS] = {
    0xFF0B0000, 0xFF0C0000, 0xFF0D0000, 0xFF0E0000,
};

static const int gem_intr[XLNX_ZYNQMP_NUM_GEMS] = {
    57, 59, 61, 63,
};

static const uint64_t uart_addr[XLNX_ZYNQMP_NUM_UARTS] = {
    0xFF000000, 0xFF010000,
};

static const int uart_intr[XLNX_ZYNQMP_NUM_UARTS] = {
    21, 22,
};

typedef struct XlnxZynqMPGICRegion {
    int region_index;
    uint32_t address;
} XlnxZynqMPGICRegion;

static const XlnxZynqMPGICRegion xlnx_zynqmp_gic_regions[] = {
    { .region_index = 0, .address = GIC_DIST_ADDR, },
    { .region_index = 1, .address = GIC_CPU_ADDR,  },
};

static uint64_t crl_apb_read(void *opaque, hwaddr addr, unsigned size)
{
  return 0;
}

static void crl_apb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    switch (addr) {
    case 0x218: /* RESET_CTRL */
      if (val & 0x10) {
        qemu_system_reset_request();
      }
      break;
    default:
      break;
    }
}

static uint64_t crf_apb_read(void *opaque, hwaddr addr, unsigned size)
{
  XlnxZynqMPState *s = (XlnxZynqMPState *)opaque;
  ARMCPU *target_cpu;
  uint64_t value;
  int j;

  switch (addr) {
  case 0x104:
    value = 0;
    for (j = 0; j < 4; j++) {
      target_cpu = (ARMCPU *)&s->apu_cpu[j];
      if (target_cpu->powered_off) {
        value |= 1 << j;
        value |= 1 << (j + 10);
      }
    }

    return value;
    break;
  default:
    return 0;
  }
}

static void crf_apb_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
  XlnxZynqMPState *s = (XlnxZynqMPState *)opaque;
  ARMCPU          *target_cpu;
  CPUState        *target_cpu_state;
  int j;

  switch (addr) {
  case 0x104: /* RST_FPD_APU register */
    for (j = 0; j < 4; j++) {
      if (!(val & (0x1 << j))) {
        // acpu[j] out of reset
        target_cpu = (ARMCPU *)&s->apu_cpu[j];
        target_cpu_state = CPU(target_cpu);
        if (target_cpu->powered_off) {
          cpu_reset(target_cpu_state);
          target_cpu->powered_off = false;
          target_cpu_state->halted = 0;
        }
      }
    }

    break;
  default:
    break;
  }
}

static const MemoryRegionOps xlnx_crl_apb = {
    .read = crl_apb_read,
    .write = crl_apb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static const MemoryRegionOps xlnx_crf_apb = {
    .read = crf_apb_read,
    .write = crf_apb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static inline int arm_gic_ppi_index(int cpu_nr, int ppi_index)
{
    return GIC_NUM_SPI_INTR + cpu_nr * GIC_INTERNAL + ppi_index;
}

static void xlnx_zynqmp_init(Object *obj)
{
    XlnxZynqMPState *s = XLNX_ZYNQMP(obj);
    int i;

    for (i = 0; i < XLNX_ZYNQMP_NUM_APU_CPUS; i++) {
        object_initialize(&s->apu_cpu[i], sizeof(s->apu_cpu[i]),
                          "cortex-a53-" TYPE_ARM_CPU);
        object_property_add_child(obj, "apu-cpu[*]", OBJECT(&s->apu_cpu[i]),
                                  &error_abort);
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_RPU_CPUS; i++) {
        object_initialize(&s->rpu_cpu[i], sizeof(s->rpu_cpu[i]),
                          "cortex-r5-" TYPE_ARM_CPU);
        object_property_add_child(obj, "rpu-cpu[*]", OBJECT(&s->rpu_cpu[i]),
                                  &error_abort);
    }

    object_initialize(&s->gic, sizeof(s->gic), TYPE_ARM_GIC);
    qdev_set_parent_bus(DEVICE(&s->gic), sysbus_get_default());

    for (i = 0; i < XLNX_ZYNQMP_NUM_GEMS; i++) {
        object_initialize(&s->gem[i], sizeof(s->gem[i]), TYPE_CADENCE_GEM);
        qdev_set_parent_bus(DEVICE(&s->gem[i]), sysbus_get_default());
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_UARTS; i++) {
        object_initialize(&s->uart[i], sizeof(s->uart[i]), TYPE_CADENCE_UART);
        qdev_set_parent_bus(DEVICE(&s->uart[i]), sysbus_get_default());
    }
}

static void xlnx_zynqmp_realize(DeviceState *dev, Error **errp)
{
    XlnxZynqMPState *s = XLNX_ZYNQMP(dev);
    MemoryRegion    *system_memory = get_system_memory();
    MemoryRegion    *crl_apb       = g_new(MemoryRegion, 1);
    MemoryRegion    *crf_apb       = g_new(MemoryRegion, 1);
    uint8_t i;
    const char *boot_cpu = s->boot_cpu ? s->boot_cpu : "apu-cpu[0]";
    qemu_irq *gic_spi = g_new(qemu_irq, GIC_NUM_SPI_INTR);
    Error *err = NULL;

    qdev_prop_set_uint32(DEVICE(&s->gic), "num-irq", GIC_NUM_SPI_INTR + 32);
    qdev_prop_set_uint32(DEVICE(&s->gic), "revision", 2);
    qdev_prop_set_uint32(DEVICE(&s->gic), "num-cpu", XLNX_ZYNQMP_NUM_APU_CPUS);
    object_property_set_bool(OBJECT(&s->gic), true, "realized", &err);
    if (err) {
        error_propagate((errp), (err));
        return;
    }

    memory_region_init_io(crl_apb, NULL, &xlnx_crl_apb, s,
                          "zynqmp-crl-apb", 0x300);
    memory_region_add_subregion(system_memory, CRL_APB_ADDR, crl_apb);
    memory_region_init_io(crf_apb, NULL, &xlnx_crf_apb, s,
                          "zynqmp-crf-apb", 0x110);
    memory_region_add_subregion(system_memory, CRF_APB_ADDR, crf_apb);

    assert(ARRAY_SIZE(xlnx_zynqmp_gic_regions) == XLNX_ZYNQMP_GIC_REGIONS);
    for (i = 0; i < XLNX_ZYNQMP_GIC_REGIONS; i++) {
        SysBusDevice *gic = SYS_BUS_DEVICE(&s->gic);
        const XlnxZynqMPGICRegion *r = &xlnx_zynqmp_gic_regions[i];
        MemoryRegion *mr = sysbus_mmio_get_region(gic, r->region_index);
        uint32_t addr = r->address;
        int j;

        sysbus_mmio_map(gic, r->region_index, addr);

        for (j = 0; j < XLNX_ZYNQMP_GIC_ALIASES; j++) {
            MemoryRegion *alias = &s->gic_mr[i][j];

            addr += XLNX_ZYNQMP_GIC_REGION_SIZE;
            memory_region_init_alias(alias, OBJECT(s), "zynqmp-gic-alias", mr,
                                     0, XLNX_ZYNQMP_GIC_REGION_SIZE);
            memory_region_add_subregion(system_memory, addr, alias);
        }
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_APU_CPUS; i++) {
        qemu_irq irq;
        char *name;

        object_property_set_int(OBJECT(&s->apu_cpu[i]), QEMU_PSCI_CONDUIT_SMC,
                                "psci-conduit", &error_abort);

        name = object_get_canonical_path_component(OBJECT(&s->apu_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /* Secondary CPUs start in PSCI powered-down state */
            object_property_set_bool(OBJECT(&s->apu_cpu[i]), true,
                                     "start-powered-off", &error_abort);
        } else {
            s->boot_cpu_ptr = &s->apu_cpu[i];
        }
        g_free(name);

        object_property_set_int(OBJECT(&s->apu_cpu[i]), GIC_BASE_ADDR,
                                "reset-cbar", &err);
        if (err) {
            error_propagate((errp), (err));
            return;
        }

        object_property_set_bool(OBJECT(&s->apu_cpu[i]), true, "realized",
                                 &err);
        if (err) {
            error_propagate((errp), (err));
            return;
        }

        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gic), i,
                           qdev_get_gpio_in(DEVICE(&s->apu_cpu[i]),
                                            ARM_CPU_IRQ));
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_PHYS_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), 0, irq);
        irq = qdev_get_gpio_in(DEVICE(&s->gic),
                               arm_gic_ppi_index(i, ARM_VIRT_TIMER_PPI));
        qdev_connect_gpio_out(DEVICE(&s->apu_cpu[i]), 1, irq);
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_RPU_CPUS; i++) {
        char *name;

        name = object_get_canonical_path_component(OBJECT(&s->rpu_cpu[i]));
        if (strcmp(name, boot_cpu)) {
            /* Secondary CPUs start in PSCI powered-down state */
            object_property_set_bool(OBJECT(&s->rpu_cpu[i]), true,
                                     "start-powered-off", &error_abort);
        } else {
            s->boot_cpu_ptr = &s->rpu_cpu[i];
        }
        g_free(name);

        object_property_set_bool(OBJECT(&s->rpu_cpu[i]), true, "reset-hivecs",
                                 &err);
        if (err != NULL) {
            error_propagate(errp, err);
            return;
        }

        object_property_set_bool(OBJECT(&s->rpu_cpu[i]), true, "realized",
                                 &err);
        if (err) {
            error_propagate((errp), (err));
            return;
        }
    }

    if (!s->boot_cpu_ptr) {
        error_setg(errp, "ZynqMP Boot cpu %s not found\n", boot_cpu);
        return;
    }

    for (i = 0; i < GIC_NUM_SPI_INTR; i++) {
        gic_spi[i] = qdev_get_gpio_in(DEVICE(&s->gic), i);
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_GEMS; i++) {
        NICInfo *nd = &nd_table[i];

        if (nd->used) {
            qemu_check_nic_model(nd, TYPE_CADENCE_GEM);
            qdev_set_nic_properties(DEVICE(&s->gem[i]), nd);
        }
        object_property_set_bool(OBJECT(&s->gem[i]), true, "realized", &err);
        if (err) {
            error_propagate((errp), (err));
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->gem[i]), 0, gem_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->gem[i]), 0,
                           gic_spi[gem_intr[i]]);
    }

    for (i = 0; i < XLNX_ZYNQMP_NUM_UARTS; i++) {
        object_property_set_bool(OBJECT(&s->uart[i]), true, "realized", &err);
        if (err) {
            error_propagate((errp), (err));
            return;
        }
        sysbus_mmio_map(SYS_BUS_DEVICE(&s->uart[i]), 0, uart_addr[i]);
        sysbus_connect_irq(SYS_BUS_DEVICE(&s->uart[i]), 0,
                           gic_spi[uart_intr[i]]);
    }

    /* Initialize plug-ins */
    plugin_init(gic_spi, 128);
    plugin_device_init();
    /* Initialize the GnatBus Master */
    gnatbus_master_init(gic_spi, 128);
    gnatbus_device_init();
}

static Property xlnx_zynqmp_props[] = {
    DEFINE_PROP_STRING("boot-cpu", XlnxZynqMPState, boot_cpu),
    DEFINE_PROP_END_OF_LIST()
};

static void xlnx_zynqmp_class_init(ObjectClass *oc, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->props = xlnx_zynqmp_props;
    dc->realize = xlnx_zynqmp_realize;

    /*
     * Reason: creates an ARM CPU, thus use after free(), see
     * arm_cpu_class_init()
     */
    dc->cannot_destroy_with_object_finalize_yet = true;
}

static const TypeInfo xlnx_zynqmp_type_info = {
    .name = TYPE_XLNX_ZYNQMP,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(XlnxZynqMPState),
    .instance_init = xlnx_zynqmp_init,
    .class_init = xlnx_zynqmp_class_init,
};

static void xlnx_zynqmp_register_types(void)
{
    type_register_static(&xlnx_zynqmp_type_info);
}

type_init(xlnx_zynqmp_register_types)
