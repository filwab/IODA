#!/bin/bash
#
# This script builds Linux kernel for IODA
# Usage: ./build-kernel.sh
#
# Note: Please cd into IODA-SOSP21-AE/ first, and then run "./build-kernel.sh"
#

IODA_TOPDIR=$(pwd)

IODA_BUILD_LOG=${IODA_TOPDIR}"/ioda-kernel-build.log"

red=`tput setaf 1`
green=`tput setaf 2`
blue=`tput setaf 4`
reset=`tput sgr0`

# Build iodaLinux-5.4.121
echo ""
echo "====> Building iodaLinux-5.4.121 ..."
echo "      Note: this is an optional step since we will use a pre-compiled version of the Linux rootfs in our VM image"
echo ""
cd ${IODA_TOPDIR}/src/iodaLinux-5.4.121
make clean >/dev/null 2>&1
cp ioda-config .config
bear make -j64 >${IODA_BUILD_LOG} 2>&1
cd ${IODA_TOPDIR}

IODA_LINUX_BIN="src/iodaLinux-5.4.121/arch/x86/boot/bzImage"

if [ -e ${IODA_LINUX_BIN} ]; then
    echo ""
    echo "===> ${green}Congrats${reset}, IODA Linux kernel is successfully built!"
    echo ""
    echo "Please check the compiled binaries:"
    echo "  - iodaLinux rootfs at [${blue}${IODA_LINUX_BIN}${reset}]"
    echo ""
else
    echo ""
    echo "===> ${red}ERROR:${reset} Failed to build IODA kernel, please check [${IODA_BUILD_LOG}]."
    echo ""
fi

# Install the kernel should be in the FEMU virtual machine
# For example, as below:

# sudo mount -t 9p -o trans=virtio hostshare /mnt/share/ -oversion=9p2000.L
# cd /mnt/share/iodaLinux-5.4.121/
# sudo make INSTALL_MOD_STRIP=1 modules_install && sudo make install && sudo poweroff