#!/bin/bash


# Remove the existing build directory, recreate it, and navigate into it
rm -rf build
mkdir build
cd build
mkdir objs

# Run cmake with the appropriate flags
cmake -DONLINE=ON ..


# Run make to build the project
make

