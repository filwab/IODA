#!/bin/bash

# Create a RAID-5 array

if [[ -b /dev/md5 ]]; then
    echo "==> WARNING: /dev/md5 already exists! Skip array create..."
    exit
fi

sudo mdadm --create /dev/md5 --chunk=64K --level=5 --raid-devices=4 /dev/nvme0n1 /dev/nvme1n1 /dev/nvme2n1 /dev/nvme3n1 --assume-clean

sleep 1

echo 32768 | sudo tee /sys/block/md5/md/stripe_cache_size
echo 16 | sudo tee /sys/block/md5/md/group_thread_cnt

