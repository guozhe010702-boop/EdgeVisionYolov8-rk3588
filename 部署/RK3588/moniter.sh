#!/bin/bash
# monitor.sh
while true; do
    clear
    echo "NPU Load:"
    cat /sys/kernel/debug/rknpu/load
    echo -e "\nCPU Usage:"
    top -bn1 | grep "Cpu(s)"
    sleep 1
done

