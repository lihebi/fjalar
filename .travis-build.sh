#!/bin/bash -v

# Fail the whole script if any command fails
set -e

# Doing this causes Travis to mysteriously fail, so don't do this.
# export SHELLOPTS

# Get some system info for debugging.
cat /proc/version
gcc --version
make --version
ls -l /lib/x86_64-linux-gnu/libc-*

# TODO: The tests ought to work even if $DAIKONDIR is not set.
export DAIKONDIR=`pwd`/../daikon

make ../daikon

make -C ../daikon compile daikon.jar kvasir

cp ArraysInStructTest.dyncomp.decls ../daikon/tests/kvasir-tests/ArraysInStructTest
cd ../daikon/tests/kvasir-tests/ArraysInStructTest
make clean summary-w-daikon
ls -l daikon-output
diff -u ArraysInStructTest.dyncomp.decls daikon-output/ArraysInStructTest.dyncomp.decls
cat daikon-output/ArraysInStructTest.dyncomp.decls

