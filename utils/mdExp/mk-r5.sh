#!/bin/bash

# Create a RAID-5 array

if [[ -b /dev/md5 ]]; then
    echo "==> WARNING: /dev/md5 already exists! Skip array create..."
    exit
fi

SSD_NUM=`ls -l /dev/nvme*n1 | wc -l`

sudo mdadm --create /dev/md5 --chunk=64K --level=5 --raid-devices=${SSD_NUM} /dev/nvme*n1 --assume-clean

sleep 1

echo 32768 | sudo tee /sys/block/md5/md/stripe_cache_size
echo 16 | sudo tee /sys/block/md5/md/group_thread_cnt

