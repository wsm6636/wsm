#!/bin/bash

echo 0 256 > /sys/kernel/debug/memguard/limit
echo 1 256 > /sys/kernel/debug/memguard/limit
echo 2 256 > /sys/kernel/debug/memguard/limit
echo 3 256 > /sys/kernel/debug/memguard/limit
echo 4 256 > /sys/kernel/debug/memguard/limit
echo 5 256 > /sys/kernel/debug/memguard/limit
echo 6 256 > /sys/kernel/debug/memguard/limit
echo 7 256 > /sys/kernel/debug/memguard/limit
cat /sys/kernel/debug/memguard/limit
