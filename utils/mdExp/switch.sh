#!/bin/bash
#--------------------------------------------------------------
# Change the system benchmark mode: IODA, Base
#--------------------------------------------------------------

if [[ $# != 1 ]]; then
    echo ""
    echo "  Usage: (set RAID+FEMU into the following modes)"
    echo ""
    echo "     - $(basename $0) [ioda | base]"
    echo ""
    exit
fi

mode=$1

# Enter IODA mode
if [[ $mode == "ioda" ]]; then
    echo ""
    echo "===> Setting up IODA mode ..."
    ./md-config 1     # set PL-io
    ./femu-config.sh 8   # set PL-win
    ./femu-config.sh 13  # set 100ms tw
    echo 2 | sudo tee /sys/block/md5/queue/nomerges
    for i in 0 1 2 3; do
        echo 2 | sudo tee /sys/block/nvme${i}n1/queue/nomerges
    done
    echo "===> In IODA mode now ... [done]"
# Enter Base mode
elif [[ $mode == "base" ]]; then
    echo ""
    echo "===> Setting up Base mode ..."
    ./md-config 0     # set normal
    ./femu-config.sh 8   # set normal
    echo 0 | sudo tee /sys/block/md5/queue/nomerges
    for i in 0 1 2 3; do
        echo 0 | sudo tee /sys/block/nvme${i}n1/queue/nomerges
    done
    echo "===> In Base mode now ... [done]"
else
    echo "mode error"
fi
