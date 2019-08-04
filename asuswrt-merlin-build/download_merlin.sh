#!/bin/bash

ASUSWRT_MERLIN_VERSION=380.70

wget https://github.com/RMerl/asuswrt-merlin/archive/${ASUSWRT_MERLIN_VERSION}.tar.gz
bsdtar -zxf ${ASUSWRT_MERLIN_VERSION}.tar.gz
mv asuswrt-merlin-${ASUSWRT_MERLIN_VERSION} ./asuswrt-merlin
