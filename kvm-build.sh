#!/usr/bin/bash
modprobe -r kvm-intel
modprobe -r kvm
make M=arch/x86/kvm
make M=arch/x86/kvm modules_install
depmod -a
modprobe kvm-intel

