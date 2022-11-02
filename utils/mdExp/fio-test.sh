#! /bin/bash

./femu-config.sh 24
echo "" > fio.log
echo "=============== FIO Stress Test Start  ==============="
echo "=============== FIO Stress Write ===============" | tee -a fio.log
sudo fio write.fio >> fio.log
echo "=============== FIO Stress Read ===============" | tee -a fio.log
sudo fio read.fio >> fio.log
echo "=============== FIO Stress Test Finish ==============="
./femu-config.sh 24
