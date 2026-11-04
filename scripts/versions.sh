#!/bin/bash
# Print the toolchain this build will use. No compilation, no downloads.
set -u
echo "kernel    : $(uname -r)"
echo "headers   : /lib/modules/$(uname -r)/build"
echo "gcc       : $(gcc -dumpfullversion) ($(gcc -dumpmachine))"
echo "make      : $(make --version | head -1)"
echo "cpus      : $(nproc)"
