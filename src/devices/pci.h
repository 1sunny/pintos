#ifndef DEVICES_PCI_H
#define DEVICES_PCI_H

#include <stdio.h>

/* NOTE(willscott): This file was imported from JOS. */

// PCI subsystem interface
enum { pci_res_bus, pci_res_mem, pci_res_io, pci_res_max };

struct pci_bus;

struct pci_func {
  struct pci_bus *bus;  // 指向设备所属的 PCI 总线
  
  uint32_t dev;         // 设备号，标识该设备在总线上的位置。
  uint32_t func;        // 设备的功能号，PCI 设备可以有多个功能。
  
  uint32_t dev_id;
  uint32_t dev_class;
  
  uint32_t reg_base[6]; // 设备的寄存器基地址，最多支持六个地址空间。
  uint32_t reg_size[6]; // 每个寄存器的大小。
  uint8_t irq_line;     // 设备使用的中断线。
};

struct pci_bus {
  struct pci_func *parent_bridge;
  uint32_t busno;
};

void pci_init (void);
void pci_func_enable(struct pci_func *func);

#endif /* devices/pci.h */
