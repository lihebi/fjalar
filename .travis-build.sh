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

date
make ../daikon
date
make -C ../daikon compile daikon.jar kvasir
date
make -C ../daikon/tests/kvasir-tests nightly-summary-w-daikon >test.log 2>&1
date
cat test.log
grep FAILED test.log > travis-fail
diff travis-fail travis-fail.goal
