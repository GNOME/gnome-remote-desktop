#!/bin/sh

rm -rf ./build
mkdir build
meson build $@
