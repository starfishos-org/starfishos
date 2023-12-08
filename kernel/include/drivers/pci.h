#pragma once

#include <common/types.h>
#include <common/list.h>
#include <drivers/resource.h>
#include <drivers/pci-regs.h>

typedef u32 pci_bus_addr_t;

struct pci_bus_region {
        pci_bus_addr_t start;
        pci_bus_addr_t end;
};

struct pci_bus {
        struct list_head node; /* Node in list of buses */
        struct pci_bus *parent; /* Parent bus this bridge is on */
        struct list_head children; /* List of child buses */
        struct list_head devices; /* List of devices on this bus */
        struct pci_dev *self; /* Bridge device as seen by parent */
        struct list_head slots; /* List of slots on this bus;
                                   protected by pci_slot_mutex */
#if 0
        struct resource *resource[PCI_BRIDGE_RESOURCE_NUM];
        struct list_head resources; /* Address space routed to this bus */
        struct resource busn_res; /* Bus numbers routed to this bus */
#endif
        struct pci_ops *ops; /* Configuration access functions */

        u64 domain;
        // void *sysdata; /* Hook for sys-specific extension */
        // struct proc_dir_entry *procdir; /* Directory entry in /proc/bus/pci
        // */

        unsigned char number; /* Bus number */
        unsigned char primary; /* Number of primary bridge */
        unsigned char max_bus_speed; /* enum pci_bus_speed */
        unsigned char cur_bus_speed; /* enum pci_bus_speed */
#ifdef CONFIG_PCI_DOMAINS_GENERIC
        int domain_nr;
#endif

        char name[48];

#if 0
        unsigned short bridge_ctl; /* Manage NO_ISA/FBB/et al behaviors */
        pci_bus_flags_t bus_flags; /* Inherited by child buses */
        struct device *bridge;
        struct device dev;
        struct bin_attribute *legacy_io; /* Legacy I/O for this bus */
        struct bin_attribute *legacy_mem; /* Legacy mem */
        unsigned int is_added : 1;
        unsigned int unsafe_warn : 1; /* warned about RW1C config write */
#endif
};

/* Low-level architecture-dependent routines */

struct pci_ops {
        int (*add_bus)(struct pci_bus *bus);
        void (*remove_bus)(struct pci_bus *bus);
        int (*read)(struct pci_bus *bus, unsigned int devfn, int where,
                    int size, u32 *val);
        int (*write)(struct pci_bus *bus, unsigned int devfn, int where,
                     int size, u32 val);
};

/*
 * ACPI needs to be able to access PCI config space before we've done a
 * PCI bus scan and created pci_bus structures.
 */
int raw_pci_read(unsigned int domain, unsigned int bus, unsigned int devfn,
                 int reg, int len, u32 *val);
int raw_pci_write(unsigned int domain, unsigned int bus, unsigned int devfn,
                  int reg, int len, u32 val);

/* For PCI devices, the region numbers are assigned this way: */
enum {
        /* #0-5: standard PCI resources */
        PCI_STD_RESOURCES,
        PCI_STD_RESOURCE_END = PCI_STD_RESOURCES + PCI_STD_NUM_BARS - 1,

        /* #6: expansion ROM resource */
        PCI_ROM_RESOURCE,

/* Device-specific resources */
#ifdef CONFIG_PCI_IOV
        PCI_IOV_RESOURCES,
        PCI_IOV_RESOURCE_END = PCI_IOV_RESOURCES + PCI_SRIOV_NUM_BARS - 1,
#endif

/* PCI-to-PCI (P2P) bridge windows */
#define PCI_BRIDGE_IO_WINDOW       (PCI_BRIDGE_RESOURCES + 0)
#define PCI_BRIDGE_MEM_WINDOW      (PCI_BRIDGE_RESOURCES + 1)
#define PCI_BRIDGE_PREF_MEM_WINDOW (PCI_BRIDGE_RESOURCES + 2)

/* CardBus bridge windows */
#define PCI_CB_BRIDGE_IO_0_WINDOW  (PCI_BRIDGE_RESOURCES + 0)
#define PCI_CB_BRIDGE_IO_1_WINDOW  (PCI_BRIDGE_RESOURCES + 1)
#define PCI_CB_BRIDGE_MEM_0_WINDOW (PCI_BRIDGE_RESOURCES + 2)
#define PCI_CB_BRIDGE_MEM_1_WINDOW (PCI_BRIDGE_RESOURCES + 3)

/* Total number of bridge resources for P2P and CardBus */
#define PCI_BRIDGE_RESOURCE_NUM 4

        /* Resources assigned to buses behind the bridge */
        PCI_BRIDGE_RESOURCES,
        PCI_BRIDGE_RESOURCE_END =
                PCI_BRIDGE_RESOURCES + PCI_BRIDGE_RESOURCE_NUM - 1,

        /* Total resources associated with a PCI device */
        PCI_NUM_RESOURCES,

        /* Preserve this for compatibility */
        DEVICE_COUNT_RESOURCE = PCI_NUM_RESOURCES,
};

/* The pci_dev structure describes PCI devices */
struct pci_dev {
        struct list_head bus_list; /* Node in per-bus list */
        struct pci_bus *bus; /* Bus this device is on */
        struct pci_bus *subordinate; /* Bus this device bridges to */

        void *sysdata; /* Hook for sys-specific extension */
        // struct proc_dir_entry *procent; /* Device entry in /proc/bus/pci */
        // struct pci_slot *slot; /* Physical slot this device is in */

        unsigned int devfn; /* Encoded device & function index */
        unsigned short vendor;
        unsigned short device;
        unsigned short subsystem_vendor;
        unsigned short subsystem_device;
        unsigned int class; /* 3 bytes: (base,sub,prog-if) */
        u8 revision; /* PCI revision, low byte of class word */
        u8 hdr_type; /* PCI header type (`multi' flag masked out) */
#ifdef CONFIG_PCIEAER
        u16 aer_cap; /* AER capability offset */
        struct aer_stats *aer_stats; /* AER stats for this device */
#endif
#ifdef CONFIG_PCIEPORTBUS
        struct rcec_ea *rcec_ea; /* RCEC cached endpoint association */
        struct pci_dev *rcec; /* Associated RCEC device */
#endif
        u32 devcap; /* PCIe Device Capabilities */
        u8 pcie_cap; /* PCIe capability offset */
        u8 msi_cap; /* MSI capability offset */
        u8 msix_cap; /* MSI-X capability offset */
        u8 pcie_mpss : 3; /* PCIe Max Payload Size Supported */
        u8 rom_base_reg; /* Config register controlling ROM */
        u8 pin; /* Interrupt pin this device uses */
        u16 pcie_flags_reg; /* Cached PCIe Capabilities Register */
        unsigned long *dma_alias_mask; /* Mask of enabled devfn aliases */
#if 0 
        struct pci_driver *driver; /* Driver bound to this device */
        u64 dma_mask; /* Mask of the bits of bus address this
                         device implements.  Normally this is
                         0xffffffff.  You only need to change
                         this if your device has broken DMA
                         or supports 64-bit transfers.  */

        struct device_dma_parameters dma_parms;

        pci_power_t current_state; /* Current operating state. In ACPI,
                                      this is D0-D3, D0 being fully
                                      functional, and D3 being off. */
#endif
        u8 pm_cap; /* PM capability offset */
        unsigned int imm_ready : 1; /* Supports Immediate Readiness */
        unsigned int pme_support : 5; /* Bitmask of states from which PME#
                                         can be generated */
        unsigned int pme_poll : 1; /* Poll device's PME status bit */
        unsigned int d1_support : 1; /* Low power state D1 is supported */
        unsigned int d2_support : 1; /* Low power state D2 is supported */
        unsigned int no_d1d2 : 1; /* D1 and D2 are forbidden */
        unsigned int no_d3cold : 1; /* D3cold is forbidden */
        unsigned int bridge_d3 : 1; /* Allow D3 for bridge */
        unsigned int d3cold_allowed : 1; /* D3cold is allowed by user */
        unsigned int mmio_always_on : 1; /* Disallow turning off io/mem
                                            decoding during BAR sizing */
        unsigned int wakeup_prepared : 1;
        unsigned int skip_bus_pm : 1; /* Internal: Skip bus-level PM */
        unsigned int ignore_hotplug : 1; /* Ignore hotplug events */
        unsigned int hotplug_user_indicators : 1; /* SlotCtl indicators
                                                     controlled exclusively by
                                                     user sysfs */
        unsigned int clear_retrain_link : 1; /* Need to clear Retrain Link
                                                bit manually */
        unsigned int d3hot_delay; /* D3hot->D0 transition time in ms */
        unsigned int d3cold_delay; /* D3cold->D0 transition time in ms */

#ifdef CONFIG_PCIEASPM
        struct pcie_link_state *link_state; /* ASPM link state */
        u16 l1ss; /* L1SS Capability pointer */
        unsigned int ltr_path : 1; /* Latency Tolerance Reporting
                                      supported from root to here */
#endif
        unsigned int pasid_no_tlp : 1; /* PASID works without TLP Prefix */
        unsigned int eetlp_prefix_path : 1; /* End-to-End TLP Prefix */
#if 0
        pci_channel_state_t error_state; /* Current connectivity state */
        struct device dev; /* Generic device interface */
#endif
        int cfg_size; /* Size of config space */

        /*
         * Instead of touching interrupt line and base address registers
         * directly, use the values stored here. They might be different!
         */
        unsigned int irq;
        struct resource resource[DEVICE_COUNT_RESOURCE]; /* I/O and memory
                                                            regions + expansion
                                                            ROMs */
        struct resource driver_exclusive_resource; /* driver exclusive resource
                                                      ranges */

#if 0 
        bool match_driver; /* Skip attaching driver */

        unsigned int transparent : 1; /* Subtractive decode bridge */
        unsigned int io_window : 1; /* Bridge has I/O window */
        unsigned int pref_window : 1; /* Bridge has pref mem window */
        unsigned int pref_64_window : 1; /* Pref mem window is 64-bit */
        unsigned int multifunction : 1; /* Multi-function device */

        unsigned int is_busmaster : 1; /* Is busmaster */
        unsigned int no_msi : 1; /* May not use MSI */
        unsigned int no_64bit_msi : 1; /* May only use 32-bit MSIs */
        unsigned int block_cfg_access : 1; /* Config space access blocked */
        unsigned int broken_parity_status : 1; /* Generates false positive
                                                  parity */
        unsigned int irq_reroute_variant : 2; /* Needs IRQ rerouting variant */
        unsigned int msi_enabled : 1;
        unsigned int msix_enabled : 1;
        unsigned int ari_enabled : 1; /* ARI forwarding */
        unsigned int ats_enabled : 1; /* Address Translation Svc */
        unsigned int pasid_enabled : 1; /* Process Address Space ID */
        unsigned int pri_enabled : 1; /* Page Request Interface */
        unsigned int is_managed : 1; /* Managed via devres */
        unsigned int is_msi_managed : 1; /* MSI release via devres installed */
        unsigned int needs_freset : 1; /* Requires fundamental reset */
        unsigned int state_saved : 1;
        unsigned int is_physfn : 1;
        unsigned int is_virtfn : 1;
        unsigned int is_hotplug_bridge : 1;
        unsigned int shpc_managed : 1; /* SHPC owned by shpchp */
        unsigned int is_thunderbolt : 1; /* Thunderbolt controller */
        /*
         * Devices marked being untrusted are the ones that can potentially
         * execute DMA attacks and similar. They are typically connected
         * through external ports such as Thunderbolt but not limited to
         * that. When an IOMMU is enabled they should be getting full
         * mappings to make sure they cannot access arbitrary memory.
         */
        unsigned int untrusted : 1;
        /*
         * Info from the platform, e.g., ACPI or device tree, may mark a
         * device as "external-facing".  An external-facing device is
         * itself internal but devices downstream from it are external.
         */
        unsigned int external_facing : 1;
        unsigned int broken_intx_masking : 1; /* INTx masking can't be used */
        unsigned int io_window_1k : 1; /* Intel bridge 1K I/O windows */
        unsigned int irq_managed : 1;
        unsigned int non_compliant_bars : 1; /* Broken BARs; ignore them */
        unsigned int is_probed : 1; /* Device probing in progress */
        unsigned int link_active_reporting : 1; /* Device capable of reporting
                                                   link active */
        unsigned int no_vf_scan : 1; /* Don't scan for VFs after IOV enablement
                                      */
        unsigned int no_command_memory : 1; /* No PCI_COMMAND_MEMORY */
        unsigned int rom_bar_overlap : 1; /* ROM BAR disable broken */
        unsigned int rom_attr_enabled : 1; /* Display of ROM attribute enabled?
                                            */
        pci_dev_flags_t dev_flags;
        atomic_t enable_cnt; /* pci_enable_device has been called */

        spinlock_t pcie_cap_lock; /* Protects RMW ops in capability accessors */
        u32 saved_config_space[16]; /* Config space saved at suspend time */
        struct hlist_head saved_cap_space;
        struct bin_attribute *res_attr[DEVICE_COUNT_RESOURCE]; /* sysfs file for
                                                                  resources */
        struct bin_attribute *res_attr_wc[DEVICE_COUNT_RESOURCE]; /* sysfs file
                                                                     for WC
                                                                     mapping of
                                                                     resources
                                                                   */

#ifdef CONFIG_HOTPLUG_PCI_PCIE
        unsigned int broken_cmd_compl : 1; /* No compl for some cmds */
#endif
#ifdef CONFIG_PCIE_PTM
        u16 ptm_cap; /* PTM Capability */
        unsigned int ptm_root : 1;
        unsigned int ptm_enabled : 1;
        u8 ptm_granularity;
#endif
#ifdef CONFIG_PCI_MSI
        void __iomem *msix_base;
        raw_spinlock_t msi_lock;
#endif
        struct pci_vpd vpd;
#ifdef CONFIG_PCIE_DPC
        u16 dpc_cap;
        unsigned int dpc_rp_extensions : 1;
        u8 dpc_rp_log_size;
#endif
#ifdef CONFIG_PCI_ATS
        union {
                struct pci_sriov *sriov; /* PF: SR-IOV info */
                struct pci_dev *physfn; /* VF: related PF */
        };
        u16 ats_cap; /* ATS Capability offset */
        u8 ats_stu; /* ATS Smallest Translation Unit */
#endif
#ifdef CONFIG_PCI_PRI
        u16 pri_cap; /* PRI Capability offset */
        u32 pri_reqs_alloc; /* Number of PRI requests allocated */
        unsigned int pasid_required : 1; /* PRG Response PASID Required */
#endif
#ifdef CONFIG_PCI_PASID
        u16 pasid_cap; /* PASID Capability offset */
        u16 pasid_features;
#endif
#ifdef CONFIG_PCI_P2PDMA
        struct pci_p2pdma __rcu *p2pdma;
#endif
#ifdef CONFIG_PCI_DOE
        struct xarray doe_mbs; /* Data Object Exchange mailboxes */
#endif
        u16 acs_cap; /* ACS Capability offset */
        phys_addr_t rom; /* Physical address if not from BAR */
        size_t romlen; /* Length if not from BAR */
        /*
         * Driver name to force a match.  Do not set directly, because core
         * frees it.  Use driver_set_override() to set or clear it.
         */
        const char *driver_override;

        unsigned long priv_flags; /* Private flags for the PCI driver */

        /* These methods index pci_reset_fn_methods[] */
        u8 reset_methods[PCI_NUM_RESET_METHODS]; /* In priority order */
#endif
};

/* "PCI MMCONFIG %04x [bus %02x-%02x]" */
#define PCI_MMCFG_RESOURCE_NAME_LEN (22 + 4 + 2 + 2)

struct pci_mmcfg_region {
        struct list_head list;
        struct resource res;
        u64 address;
        char *virt;
        u16 segment;
        u8 start_bus;
        u8 end_bus;
        char name[PCI_MMCFG_RESOURCE_NAME_LEN];
};

/* add memeory config */
struct pci_mmcfg_region *pci_mmconfig_add(int segment, int start, int end,
                                          u64 addr);
struct pci_mmcfg_region *pci_mmconfig_lookup(int segment, int bus);

int pci_read_config_byte(const struct pci_dev *dev, int where, u8 *val);
int pci_read_config_word(const struct pci_dev *dev, int where, u16 *val);
int pci_read_config_dword(const struct pci_dev *dev, int where, u32 *val);
int pci_write_config_byte(const struct pci_dev *dev, int where, u8 val);
int pci_write_config_word(const struct pci_dev *dev, int where, u16 val);
int pci_write_config_dword(const struct pci_dev *dev, int where, u32 val);

/* Parse memory configuration in ACPI table */
void arch_pci_mmcfg_init();

static inline resource_size_t resource_size(const struct resource *res)
{
        return res->end - res->start + 1;
}
/*
 * These helpers provide future and backwards compatibility
 * for accessing popular PCI BAR info
 */
#define pci_resource_n(dev, bar)     (&(dev)->resource[(bar)])
#define pci_resource_start(dev, bar) (pci_resource_n(dev, bar)->start)
#define pci_resource_end(dev, bar)   (pci_resource_n(dev, bar)->end)
#define pci_resource_flags(dev, bar) (pci_resource_n(dev, bar)->flags)
#define pci_resource_len(dev, bar)                             \
        (pci_resource_end((dev), (bar)) ?                      \
                 resource_size(pci_resource_n((dev), (bar))) : \
                 0)

enum pci_bar_type {
        pci_bar_unknown, /* Standard PCI BAR probe */
        pci_bar_io, /* An I/O port BAR */
        pci_bar_mem32, /* A 32-bit memory BAR */
        pci_bar_mem64, /* A 64-bit memory BAR */
};

/* Probe devices from a region */
void arch_pci_probe_devices();

typedef void (*pci_bus_traverse_fn)(struct pci_dev *pdev);
void pci_buses_traverse_all(pci_bus_traverse_fn func);
u16 pci_find_dvsec_capability(struct pci_dev *dev, u16 vendor, u16 dvsec);

/* Init pcie devices */
int pci_setup_device(struct pci_dev *dev);
void pci_setup_devices();
