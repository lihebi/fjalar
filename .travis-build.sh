#!/bin/bash -v

# TODO: The tests ought to work even if $DAIKONDIR is not set.
export DAIKONDIR=`pwd`/../daikon

make build

make doc

## Valgrind doesn't pass its own tests ("make test").  So we should have a
## version of the target that determines the current operating system,
## compares the observed failures to the expected failures (those suffered
## by "make test" on a fresh Valgrind installation on that OS), and the
## overall target only fails if the set of failing tests is different.
# make test

## Kvasir does not currently pass all its tests on Ubuntu 14.04 which is
## used by Travis.  So, "make daikon-test" fails.  Reinstate "daikon-test"
## as soon as we have improved Kvasir and/or its test suite so that "make
## daikon-test" passes.
# make daikon-test

