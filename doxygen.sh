#!/bin/sh

set -e # exit on errors

SRCDIR=`dirname $0`
test -z "$SRCDIR" && SRCDIR=.
cd "$SRCDIR"

export SRCDIR
rm -rf doxygen_doc
doxygen doxygen.cfg
echo Timestamp > doxygen_doc/spice.tag
