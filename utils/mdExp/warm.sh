#!/bin/bash

echo ""
echo "==> [1/3] Disable FEMU all delay emulation"
echo ""
# disable all delay emulation
./femu-config.sh 2 >/dev/null 2>&1
./femu-config.sh 4 >/dev/null 2>&1

echo ""
echo "==> [2/3] Warming up the array to reach steady state (this will take 2 minutes)..."
echo ""
# run steadystate ssd workload
sed -i -e '/^runtime=/ c \runtime=60' write.fio
sudo fio write.fio >/dev/null 2>&1
sed -i -e '/^runtime=/ c \runtime=300' write.fio

echo ""
echo "==> [3/3] Enable FEMU all delay emulation"
echo ""
# enable all delay emulation
./femu-config.sh 1 >/dev/null 2>&1
./femu-config.sh 3 >/dev/null 2>&1


echo ""
echo ""
echo "-------------------------------------------------"
echo "Great! SSD have reached steady state!"
echo "-------------------------------------------------"
echo ""
echo "====> Now ready to run experiments!"
echo ""
