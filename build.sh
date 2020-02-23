#!/usr/bin/bash
export LOCALVERSION=
make -j 16 && make modules -j 16 && make modules_install -j 16 && make install

