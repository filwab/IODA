#!/bin/bash

IODA_IMGDIR="./images/"
# IODA_KERNEL="src/iodaLinux-5.4.121/arch/x86/boot/bzImage"
IODA_FEMU="src/iodaFEMU-b13b482/build-femu/x86_64-softmmu/qemu-system-x86_64"

echo 2 | sudo tee /sys/kernel/mm/ksm/run >/dev/null 2>&1

echo "===> Booting the IODA Virtual Machine..."
#sleep 3

    # -kernel "${IODA_KERNEL}" \
    # -append "root=/dev/vda1 console=ttyS0,115200n8 console=tty0" \

sudo ${IODA_FEMU} \
    -name "iodaVM" \
    -cpu host \
    -smp 48 \
    -m 24G \
    -enable-kvm \
    -boot menu=on \
    -drive file=${IODA_IMGDIR}/ioda.qcow2,if=virtio,cache=none,format=qcow2 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -device femu,devsz_mb=16384,femu_mode=1 \
    -netdev user,id=user0,hostfwd=tcp::10101-:22 \
    -device virtio-net-pci,netdev=user0 \
    -nographic | tee ./ioda-femu.log 2>&1 \

    # -vnc :1     # for debug kernel boot
