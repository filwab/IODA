#! /bin/bash

# git clone git@github.com:brendangregg/FlameGraph.git --depth=1

CONFIG=rand_write

process="md5_raid5"
processid=$(ps -ef | grep ${process} | grep -v grep | awk '{print $2}')
echo "process name is ${process}, tid is ${processid}"

rm -rf perf.*
sudo perf record -F 99 --call-graph dwarf -p ${processid} -- fio ${CONFIG}.fio
sudo perf script -i perf.data > perf.unfold
./FlameGraph/stackcollapse-perf.pl perf.unfold > perf.folded
./FlameGraph/flamegraph.pl perf.folded > ${CONFIG}.svg
echo "flame svg file generated: ${CONFIG}.svg"
rm -rf perf.*
