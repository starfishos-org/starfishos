#pragma once

/* QEMU virtio device vendor ID */
#define PCI_VENDOR_ID_REDHAT       0x1af4

/* Legacy virtio device IDs */
#define PCI_DEVICE_ID_VIRTIO_NET   0x1000  /* Network device */
#define PCI_DEVICE_ID_VIRTIO_BLOCK 0x1001  /* Block device */
#define PCI_DEVICE_ID_VIRTIO_BLLN  0x1002  /* Balloon device */
#define PCI_DEVICE_ID_VIRTIO_CONS  0x1003  /* Console device */
#define PCI_DEVICE_ID_VIRTIO_SCSI  0x1004  /* SCSI device */
#define PCI_DEVICE_ID_VIRTIO_RNG   0x1005  /* RNG device */
#define PCI_DEVICE_ID_VIRTIO_9P    0x1009  /* 9P transport */
#define PCI_DEVICE_ID_VIRTIO_VSOCK 0x1012  /* virtio-vsock */

/* Modern virtio device ID range: 0x1040-0x10ef */
#define PCI_DEVICE_ID_VIRTIO_BASE  0x1040  /* Base ID for modern devices */

/* Inter-VM shared memory device */
#define PCI_DEVICE_ID_IVSHMEM      0x1110  /* ivshmem device */
