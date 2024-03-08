#!/bin/bash

rm -r build
make

current_hostname=$(hostname)

if [ "$current_hostname" == "yy-OptiPlex-7090" ]; then
    param="-s 0"
elif [ "$current_hostname" == "markchen-OptiPlex-7090" ]; then
    param="-s 1"
else
    echo "Unknown hostname, exiting script."
    exit 1
fi

./build/hopa_cp -b 01:00.1 -l 0-1 -- $param