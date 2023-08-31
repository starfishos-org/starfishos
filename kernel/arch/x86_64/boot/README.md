# Booting ChCore under UEFI enabled x86-64 pc

### Booting ChCore using local bootloader

This part assumes that you are using **Ubuntu**.

1. Build x86-64 ChCore image.
   ```
   ./docker_build.sh x86-64 full
   ```
2. Make sure you have **grub2** installed.

   ```
   sudo apt-get update
   sudo apt-get install grub2
   ```

3. Find the **boot partition** of your disk.

   * Use the following instruction to get the boot partition **name** and its **mount point**, e.g. /dev/sda1 or /dev/sdb1, /boot or /boot/efi.

   ```
   lsblk
   ```
   * Get the **uuid** of the boot partition of your disk.
   ```
   ls -l /dev/disk/by-uuid
   ```
4. Copy ```kernel.img``` to the boot partition of your pc.
   ```
   sudo cp ${kernel.img} ${boot-partition-mount-point} // e.g. sudo cp build/kernel.img /boot
   ```

5. Modify ```/etc/default/grub```

   * ```GRUB_TIMEOUT=10```

   * Comment out anything related to ```TIMEOUT_STYLE_HIDDEN``` ( In my pc, it looks like ```GRUB_TIMEOUT_STYLE=hidden``` )

6. Modify ```/etc/grub.d/40_custom```

   Append the following code to this file. ( remember to replace ${uuid_of_boot_partition} with the real uuid of your boot partition )

   ```\# Boot menu entries
   # ChCore boot menu entries
   menuentry "IPADS ChCore x86-64" {
   	search --no-floppy --fs-uuid --set=root ${uuid_of_boot_partition}
   	multiboot2 /kernel.img
   }
   ```

7. ```sudo chmod +x /etc/grub.d/40_custom```

8. ```sudo update-grub```

9. Restart your pc and boot ChCore.

### Booting ChCore using bootable UDISK

1. Follow the step 1 of https://my.oschina.net/abcfy2/blog/491140 to format your UDISK.
2. Follow the step 2 of https://my.oschina.net/abcfy2/blog/491140 to install grub2 on your UDISK.
3. Copy ``kernel.img`` to ``/boot`` of your UDISK.
4. Modify grub.cfg to the following code

```
# ChCore boot menu entries
menuentry "IPADS ChCore x86-64" {
	multiboot2 /boot/kernel.img
}
```

5. Restart your pc and boot using this UDISK.
