#!/bin/bash
mkdir -p build
cd build
cmake -D CMAKE_BUILD_TYPE:STRING=Debug ..
make
