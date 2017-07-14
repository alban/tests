#!/usr/bin/env python
# Copyright (c) PLUMgrid, Inc.
# Licensed under the Apache License, Version 2.0 (the "License")

from bcc import BPF

bpf_text="""
#include <uapi/linux/ptrace.h>

int kprobe__sys_newlstat(struct pt_regs *ctx) {
    char __user *filename = (char __user *)PT_REGS_PARM1(ctx);
    bpf_trace_printk("lstat! %llu ; %s\\n", bpf_ktime_get_ns(), filename);
    return 0;
}
"""

BPF(text=bpf_text).trace_print()
