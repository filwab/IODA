#!/bin/bash
#-------------------------------------------------------------------------------
# Script to dynamically config (enable/disable) FEMU features
#-------------------------------------------------------------------------------

FEMU_FLIP_CMD_OPCODE="0xef"

cmd=$1

if [[ $# != 1 || ${cmd} -lt 1 || ${cmd} -gt 24 || ( ${cmd} -gt 4 && ${cmd} -lt 8 ) || ( ${cmd} -gt 9 && ${cmd} -lt 12 ) || ( ${cmd} -gt 18 && ${cmd} -lt 24 ) ]]; then
    echo ""
    echo "Usage: $(basename $0) [1-4,8-9,12-18,24]"
    echo "Latency:"
    echo "  1 : enable_gc_delay,      2 : disable_gc_delay"
    echo "  3 : enable_delay_emu,     4 : disable_delay_emu"
    echo "IODA:"
    echo "  8 : enable_gc_sync,       9 : disable_gc_sync"
    echo "IODA TW:"
    echo "  12: tw=1sec,              13: tw=100ms"
    echo "  14: tw=2sec,              15: tw=10ms"
    echo "  16: tw=40ms,              17: tw=200ms"
    echo "  18: tw=400ms"
    echo "LOG:"
    echo "  24: toggle_nand_utilization"
    echo ""
    exit
fi

for i in $(seq 0 3); do
    sudo nvme admin-passthru --opcode=${FEMU_FLIP_CMD_OPCODE} --cdw10=${cmd} /dev/nvme$i >/dev/null 2>&1
done
