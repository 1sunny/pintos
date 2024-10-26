#include "devices/pci.h"
#include "devices/e100.h"
#include "devices/pcireg.h"
#include <ctype.h>
#include <debug.h>
#include <stdio.h>
#include <string.h>
#include "threads/io.h"

/* The code in this file is an interface to the PCI subsystem,
   and provides access to PCI-connected devices.
   NOTE(willscott): This file was migrated from the JOS distribution. */

// Flag to do "lspci" at bootup
static int pci_show_devs = 1;
static int pci_show_addrs = 0;

// PCI "configuration mechanism one"
static uint32_t pci_conf1_addr_ioport = 0x0cf8;
static uint32_t pci_conf1_data_ioport = 0x0cfc;

// Forward declarations
static int pci_bridge_attach(struct pci_func *pcif);

// PCI driver table
struct pci_driver {
	uint32_t key1, key2;
	int (*attachfn) (struct pci_func *pcif);
};

struct pci_driver pci_attach_class[] = {
	{ PCI_CLASS_BRIDGE, PCI_SUBCLASS_BRIDGE_PCI, &pci_bridge_attach },
	{ 0, 0, 0 },
};

struct pci_driver pci_attach_vendor[] = {
	{ E100_VENDORID, E100_QEMU_DEVICEID, &pci_e100_attach },
	{ E100_VENDORID, E100_BOCHS_DEVICEID, &pci_e100_attach },
	{ 0, 0, 0 },
};

// 设置要访问的PCI配置空间地址.PCI设备通过总线号(bus),设备号(dev),功能号(func),
// 以及配置空间偏移量(offset)唯一标识
static void
pci_conf1_set_addr(uint32_t bus,
		   uint32_t dev,
		   uint32_t func,
		   uint32_t offset)
{
	ASSERT(bus < 256);
	ASSERT(dev < 32);
	ASSERT(func < 8);
	ASSERT(offset < 256);
	ASSERT((offset & 0x3) == 0);
	// 设置最高位,表示这是一个配置空间访问请求
	uint32_t v = (1 << 31) |		// config-space
		(bus << 16) | (dev << 11) | (func << 8) | (offset);
	outl(pci_conf1_addr_ioport, v);
}

// 读取指定PCI设备的配置空间.调用pci_conf1_set_addr设置目标设备的地址,
// 然后通过inl从pci_conf1_data_ioport读取配置数据
static uint32_t
pci_conf_read(struct pci_func *f, uint32_t off)
{
	pci_conf1_set_addr(f->bus->busno, f->dev, f->func, off);
	return inl(pci_conf1_data_ioport);
}

static void
pci_conf_write(struct pci_func *f, uint32_t off, uint32_t v)
{
	pci_conf1_set_addr(f->bus->busno, f->dev, f->func, off);
	outl(pci_conf1_data_ioport, v);
}

static int __attribute__((warn_unused_result))
pci_attach_match(uint32_t key1, uint32_t key2,
		 struct pci_driver *list, struct pci_func *pcif)
{
	uint32_t i;
	
	for (i = 0; list[i].attachfn; i++) {
		if (list[i].key1 == key1 && list[i].key2 == key2) {
			int r = list[i].attachfn(pcif);
			if (r > 0)
				return r;
			if (r < 0)
				printf("pci_attach_match: attaching "
					"%x.%x (%p): %d\n",
					key1, key2, list[i].attachfn, r);
		}
	}
	return 0;
}

// 根据设备的类代码或厂商 ID/设备 ID来匹配驱动。
// 通过 pci_attach_match 先根据设备类代码（dev_class 和 subclass）尝试挂载驱动。
// 如果找不到匹配的类驱动，则尝试根据厂商 ID 和产品 ID（dev_id）挂载驱动。
static int
pci_attach(struct pci_func *f)
{
	return
		pci_attach_match(PCI_CLASS(f->dev_class), 
				 PCI_SUBCLASS(f->dev_class),
				 &pci_attach_class[0], f) ||
		pci_attach_match(PCI_VENDOR(f->dev_id), 
				 PCI_PRODUCT(f->dev_id),
				 &pci_attach_vendor[0], f);
}

static const char *pci_class[] = 
{
	[0x0] = "Unknown",
	[0x1] = "Storage controller",
	[0x2] = "Network controller",
	[0x3] = "Display controller",
	[0x4] = "Multimedia device",
	[0x5] = "Memory controller",
	[0x6] = "Bridge device",
};

static void 
pci_print_func(struct pci_func *f)
{
	const char *class = pci_class[0];
	if (PCI_CLASS(f->dev_class) < sizeof(pci_class) / sizeof(pci_class[0]))
		class = pci_class[PCI_CLASS(f->dev_class)];

	printf("PCI: %02x:%02x.%d: %04x:%04x: class: %x.%x (%s) irq: %d\n",
		f->bus->busno, f->dev, f->func,
		PCI_VENDOR(f->dev_id), PCI_PRODUCT(f->dev_id),
		PCI_CLASS(f->dev_class), PCI_SUBCLASS(f->dev_class), class,
		f->irq_line);
}

// 扫描指定的PCI总线,检测连接在总线上的设备,并尝试为每个设备挂载对应的驱动程序
// 遍历总线上的每个设备(最多32个设备)
// 检查设备是否存在(通过读取配置空间PCI_ID_REG,如果厂商ID为0xffff,表示无设备)
// 检测到设备后,调用pci_attach为设备寻找并挂载相应的驱动程序
static int 
pci_scan_bus(struct pci_bus *bus)
{
	int totaldev = 0;
	struct pci_func df;
	memset(&df, 0, sizeof(df));
	df.bus = bus;
	
	for (df.dev = 0; df.dev < 32; df.dev++) {
		uint32_t bhlc = pci_conf_read(&df, PCI_BHLC_REG);
		if (PCI_HDRTYPE_TYPE(bhlc) > 1)	    // Unsupported or no device
			continue;
		
		totaldev++;
		
		struct pci_func f = df;
		for (f.func = 0; f.func < (PCI_HDRTYPE_MULTIFN(bhlc) ? 8 : 1);
		     f.func++) {
			struct pci_func af = f;
			
			af.dev_id = pci_conf_read(&f, PCI_ID_REG);
			if (PCI_VENDOR(af.dev_id) == 0xffff)
				continue;
			
			uint32_t intr = pci_conf_read(&af, PCI_INTERRUPT_REG);
			af.irq_line = PCI_INTERRUPT_LINE(intr); // 相当于要用这个作为中断号?
			
			af.dev_class = pci_conf_read(&af, PCI_CLASS_REG);
			if (pci_show_devs)
				pci_print_func(&af);
			pci_attach(&af);
		}
	}
	
	return totaldev;
}

static int
pci_bridge_attach(struct pci_func *pcif)
{
	uint32_t ioreg  = pci_conf_read(pcif, PCI_BRIDGE_STATIO_REG);
	uint32_t busreg = pci_conf_read(pcif, PCI_BRIDGE_BUS_REG);
	
	if (PCI_BRIDGE_IO_32BITS(ioreg)) {
		printf("PCI: %02x:%02x.%d: 32-bit bridge IO not supported.\n",
			pcif->bus->busno, pcif->dev, pcif->func);
		return 0;
	}
	
	struct pci_bus nbus;
	memset(&nbus, 0, sizeof(nbus));
	nbus.parent_bridge = pcif;
	nbus.busno = (busreg >> PCI_BRIDGE_BUS_SECONDARY_SHIFT) & 0xff;
	
	if (pci_show_devs)
		printf("PCI: %02x:%02x.%d: bridge to PCI bus %d--%d\n",
			pcif->bus->busno, pcif->dev, pcif->func,
			nbus.busno,
			(busreg >> PCI_BRIDGE_BUS_SUBORDINATE_SHIFT) & 0xff);
	
	pci_scan_bus(&nbus);
	return 1;
}

// External PCI subsystem interface
// 功能：启用 PCI 设备的 I/O 和内存访问功能。向 PCI 命令状态寄存器 写入启用标志，开启设备的 I/O、内存和总线主控功能。
// BAR（Base Address Registers）：为设备的内存或 I/O 地址分配基地址，并计算设备所需的内存或 I/O 地址大小。
void
pci_func_enable(struct pci_func *f)
{
	pci_conf_write(f, PCI_COMMAND_STATUS_REG,
		       PCI_COMMAND_IO_ENABLE |
		       PCI_COMMAND_MEM_ENABLE |
		       PCI_COMMAND_MASTER_ENABLE);
	
	uint32_t bar_width;
	uint32_t bar;
	for (bar = PCI_MAPREG_START; bar < PCI_MAPREG_END;
	     bar += bar_width)
	{
		uint32_t oldv = pci_conf_read(f, bar);
		
		bar_width = 4;
		pci_conf_write(f, bar, 0xffffffff);
		uint32_t rv = pci_conf_read(f, bar);
		
		if (rv == 0)
			continue;
		
		int regnum = PCI_MAPREG_NUM(bar);
		uint32_t base, size;
		if (PCI_MAPREG_TYPE(rv) == PCI_MAPREG_TYPE_MEM) {
			if (PCI_MAPREG_MEM_TYPE(rv) == PCI_MAPREG_MEM_TYPE_64BIT)
				bar_width = 8;
			
			size = PCI_MAPREG_MEM_SIZE(rv);
			base = PCI_MAPREG_MEM_ADDR(oldv);
			if (pci_show_addrs)
				printf("  mem region %d: %d bytes at 0x%x\n",
					regnum, size, base);
		} else {
			size = PCI_MAPREG_IO_SIZE(rv);
			base = PCI_MAPREG_IO_ADDR(oldv);
			if (pci_show_addrs)
				printf("  io region %d: %d bytes at 0x%x\n",
					regnum, size, base);
		}
		
		pci_conf_write(f, bar, oldv);
		f->reg_base[regnum] = base;
		f->reg_size[regnum] = size;
		
		if (size && !base)
			printf("PCI device %02x:%02x.%d (%04x:%04x) "
				"may be misconfigured: "
				"region %d: base 0x%x, size %d\n",
				f->bus->busno, f->dev, f->func,
				PCI_VENDOR(f->dev_id), PCI_PRODUCT(f->dev_id),
				regnum, base, size);
	}
}

void
pci_init(void)
{
  static struct pci_bus root_bus;
  memset(&root_bus, 0, sizeof(root_bus));
	
  pci_scan_bus(&root_bus);
}
