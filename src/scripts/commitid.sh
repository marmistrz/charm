#!/bin/bash

# Get a string describing the current code, and compare it to the
# recorded value in VERSION. Copy over that if it's changed, so that
# Make sees a more recent file.

VOLD=""
if [ -r VERSION ]
then
    VOLD=`cat VERSION`
fi

git describe --long --dirty > VERSION.new || touch VERSION.new

VNEW=`cat VERSION.new`

if [ "$VOLD" != "$VNEW" ]
then
    cp VERSION.new VERSION
    echo Copying VERSION.new = $VNEW over VERSION = $VOLD
fi
