#!/bin/bash
rm -r build
make
./build/hopa -b 03:00.1  -b  auxiliary:mlx5_core.sf.4 -b auxiliary:mlx5_core.sf.5 -b  auxiliary:mlx5_core.sf.2 -b  auxiliary:mlx5_core.sf.3 -l 0-2
