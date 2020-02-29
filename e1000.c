#include "types.h"
#include "defs.h"
#include "assert.h"
#include "pci.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "traps.h"
#include "mbuf.h"
#include "e1000_dev.h"

#define RX_DESC_NUM 16
#define TX_DESC_NUM 16

struct e1000 {
  uint32_t mmio_base;
  struct rx_desc rx_ring[RX_DESC_NUM] __attribute__((aligned(16)));
  struct tx_desc tx_ring[TX_DESC_NUM] __attribute__((aligned(16)));
  struct mbuf *rx_mbufs[RX_DESC_NUM];
  struct mbuf *tx_mbufs[TX_DESC_NUM];
  uint8_t addr[6];
  uint8_t irq;
  struct e1000 *next;
};

static struct e1000 *devices;

unsigned int
e1000_reg_read(struct e1000 *dev, uint16_t reg)
{
  return *(volatile uint32_t *)(dev->mmio_base + reg);
}

void
e1000_reg_write(struct e1000 *dev, uint16_t reg, uint32_t val)
{
  *(volatile uint32_t *)(dev->mmio_base + reg) = val;
}

static uint16_t
e1000_eeprom_read(struct e1000 *dev, uint8_t addr)
{
  uint32_t eerd;

  e1000_reg_write(dev, E1000_EERD, E1000_EERD_READ | addr << E1000_EERD_ADDR);
  while (!((eerd = e1000_reg_read(dev, E1000_EERD)) & E1000_EERD_DONE))
    microdelay(1);

  return (uint16_t)(eerd >> E1000_EERD_DATA);
}

static void
e1000_read_addr_from_eeprom(struct e1000 *dev, uint8_t *dst)
{
  uint16_t data;

  for (int n = 0; n < 3; n++) {
    data = e1000_eeprom_read(dev, n);
    dst[n*2+0] = (data & 0xff);
    dst[n*2+1] = (data >> 8) & 0xff;
  }
}

static uint32_t
e1000_resolve_mmio_base(struct pci_func *pcif)
{
  uint32_t mmio_base = 0;

  for (int n = 0; n < 6; n++) {
    if (pcif->reg_base[n] > 0xffff) {
      assert(pcif->reg_size[n] == (1<<17));
      mmio_base = pcif->reg_base[n];
      break;
    }
  }
  return mmio_base;
}

static void
e1000_rx_init(struct e1000 *dev)
{
  // initialize rx descriptors/mbufs
  for(int n = 0; n < RX_DESC_NUM; n++) {
    memset(&dev->rx_ring[n], 0, sizeof(struct rx_desc));
    dev->rx_mbufs[n] = mbufalloc(0);
    dev->rx_ring[n].addr = (uint64_t)V2P(dev->rx_mbufs[n]->head);
  }
  // setup rx descriptors
  uint64_t base = (uint64_t)(V2P(dev->rx_ring));
  e1000_reg_write(dev, E1000_RDBAL, (uint32_t)(base & 0xffffffff));
  e1000_reg_write(dev, E1000_RDBAH, (uint32_t)(base >> 32));
  cprintf("e1000: RDBAH/RDBAL = %p:%p\n", e1000_reg_read(dev, E1000_RDBAH), e1000_reg_read(dev, E1000_RDBAL));
  // rx descriptor lengh
  e1000_reg_write(dev, E1000_RDLEN, (uint32_t)(RX_DESC_NUM * 16));
  // setup head/tail
  e1000_reg_write(dev, E1000_RDH, 0);
  e1000_reg_write(dev, E1000_RDT, RX_DESC_NUM - 1);
  // set tx control register
  e1000_reg_write(dev, E1000_RCTL, (
    E1000_RCTL_SBP        | /* store bad packet */
    E1000_RCTL_UPE        | /* unicast promiscuous enable */
    E1000_RCTL_MPE        | /* multicast promiscuous enab */
    E1000_RCTL_RDMTS_HALF | /* rx desc min threshold size */
    E1000_RCTL_SECRC      | /* Strip Ethernet CRC */
    E1000_RCTL_LPE        | /* long packet enable */
    E1000_RCTL_BAM        | /* broadcast enable */
    E1000_RCTL_SZ_2048    | /* rx buffer size 2048 */
    0)
  );
}

static void
e1000_tx_init(struct e1000 *dev)
{
  // Initialize tx descpriptors/mbufs
  for (int n = 0; n < TX_DESC_NUM; n++) {
    memset(&dev->tx_ring[n], 0, sizeof(struct tx_desc));
    dev->tx_mbufs[n] = 0;
  }
  // setup tx descriptors
  uint64_t base = (uint64_t)(V2P(dev->tx_ring));
  e1000_reg_write(dev, E1000_TDBAL, (uint32_t)(base & 0xffffffff));
  e1000_reg_write(dev, E1000_TDBAH, (uint32_t)(base >> 32) );
  cprintf("e1000: TDBAH/TDBAL = %p:%p\n", e1000_reg_read(dev, E1000_TDBAH), e1000_reg_read(dev, E1000_TDBAL));
  // tx buffer length
  e1000_reg_write(dev, E1000_TDLEN, (uint32_t)(TX_DESC_NUM * sizeof(struct tx_desc)));
  // setup head/tail
  e1000_reg_write(dev, E1000_TDH, 0);
  e1000_reg_write(dev, E1000_TDT, TX_DESC_NUM);
  // set tx control register
  e1000_reg_write(dev, E1000_TCTL, (
    E1000_TCTL_EN  | /* enable tx */
    E1000_TCTL_PSP | /* pad short packets */
    0)
  );
}

static void
e1000_rx(struct e1000 *dev)
{
  cprintf("e1000: dev=%p\n", dev);
  while (1) {
    uint32_t tail = (e1000_reg_read(dev, E1000_RDT)+1) % RX_DESC_NUM;
    struct rx_desc *desc = &dev->rx_ring[tail];
    if (!(desc->status & E1000_RXD_STAT_DD)) {
      /* EMPTY */
      break;
    }
    do {
      if (desc->length < 60) {
        cprintf("e1000: short packet (%d bytes)\n", desc->length);
        break;
      }
      if (!(desc->status & E1000_RXD_STAT_EOP)) {
        cprintf("e1000: not EOP! this driver does not support packet that do not fit in one buffer\n");
        break;
      }
      if (desc->errors) {
        cprintf("e1000: rx errors (0x%x)\n", desc->errors);
        break;
      }
      struct mbuf *mbuf = dev->rx_mbufs[tail];
      cprintf("e1000: %d bytes received\n", desc->length);
      hexdump(mbuf->head, desc->length);
    } while (0);
    desc->status = (uint16_t)(0);
    e1000_reg_write(dev, E1000_RDT, tail);
  }
}

void
e1000_intr(void)
{
  struct e1000 *dev;
  uint32_t icr;
  cprintf("\n[e1000_intr]\n");
  for (dev = devices; dev; dev = dev->next) {
    icr = e1000_reg_read(dev, E1000_ICR);
    if (icr & E1000_ICR_RXT0) {
      e1000_rx(dev);
      // clear pending interrupts
      e1000_reg_read(dev, E1000_ICR);
    }
  }
}

int
e1000_init(struct pci_func *pcif)
{
  struct e1000 *dev = (struct e1000*)kalloc();

  // Resolve MMIO base address
  dev->mmio_base = e1000_resolve_mmio_base(pcif);
  assert(dev->mmio_base);
  cprintf("e1000: mmio_base=%x\n", dev->mmio_base);
  // Read HW address from EEPROM
  e1000_read_addr_from_eeprom(dev, dev->addr);
  cprintf("e1000: addr=%x:%x:%x:%x:%x:%x\n", dev->addr[0], dev->addr[1], dev->addr[2], dev->addr[3], dev->addr[4], dev->addr[5]);
  // Register I/O APIC
  dev->irq = pcif->irq_line;
  ioapicenable(dev->irq, ncpu - 1);
  // Link Up
  e1000_reg_write(dev, E1000_CTL, e1000_reg_read(dev, E1000_CTL) | E1000_CTL_SLU);
  // Initialize Multicast Table Array
  for (int n = 0; n < 128; n++)
    e1000_reg_write(dev, E1000_MTA+(n<<2), 0);
  // Enable interrupts
  e1000_reg_write(dev, E1000_IMS, E1000_IMS_RXT0);
  // Clear existing pending interrupts
  e1000_reg_read(dev, E1000_ICR);
  // Initialize RX/TX
  e1000_rx_init(dev);
  e1000_tx_init(dev);
  // Enable RX
  e1000_reg_write(dev, E1000_RCTL, e1000_reg_read(dev, E1000_RCTL) | E1000_RCTL_EN);

  dev->next = devices ? devices : 0;
  devices = dev;

  return 0;
}
