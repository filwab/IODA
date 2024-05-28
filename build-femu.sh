#!/bin/bash
#
# This script builds FEMU for IODA
# Usage: ./build.sh
#
# Note: Please cd into IODA-SOSP21-AE/ first, and then run "./build-femu.sh"
#

num=${1:-8}

IODA_TOPDIR=$(pwd)

IODA_BUILD_LOG=${IODA_TOPDIR}"/ioda-femu-build.log"

red=`tput setaf 1`
green=`tput setaf 2`
blue=`tput setaf 4`
reset=`tput sgr0`

FTL_FILE="${IODA_TOPDIR}/src/iodaFEMU-b13b482/hw/femu/bbssd/ftl.h"
sed -i "s/^#define SSD_NUM .*/#define SSD_NUM (${num})/" ${FTL_FILE}

# Build iodaFEMU-b13b482
echo ""
echo "====> Building iodaFEMU ..."
echo ""
cd ${IODA_TOPDIR}/src/iodaFEMU-b13b482
mkdir -p build-femu
cd build-femu
cp ../femu-scripts/femu-compile.sh .
make clean >/dev/null 2>&1
./femu-compile.sh >${IODA_BUILD_LOG} 2>&1
cd ${IODA_TOPDIR}

IODA_FEMU_BIN="src/iodaFEMU-b13b482/build-femu/x86_64-softmmu/qemu-system-x86_64"

if [ -e ${IODA_FEMU_BIN} ]; then
    echo ""
    echo "===> ${green}Congrats${reset}, IODA FEMU is successfully built!"
    echo ""
    echo "Please check the compiled binaries:"
    echo "  - iodaFEMU-b13b482 at [${blue}${IODA_FEMU_BIN}${reset}]"
    echo ""
else
    echo ""
    echo "===> ${red}ERROR:${reset} Failed to build IODA FEMU, please check [${IODA_BUILD_LOG}]."
    echo ""
fi

