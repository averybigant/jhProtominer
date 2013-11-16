jhProtominer
============

jhProtominer is a high performance miner for Protoshares. It uses a different algorithm to allow arbitary memory usage per thread at the cost of mining speed.

This version is based on Tydus's linux port of jhProtominer v0.1c and it also includes wangchun's improvement.

USAGE
=====

make CFLAGS="-O3 -march=native"
./jhProtominer --help
