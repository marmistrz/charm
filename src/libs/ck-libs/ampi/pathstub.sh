#!/bin/bash
DIR=$(dirname $0)
FN=$(basename $0)

$DIR/../$FN.ampi "$@"
