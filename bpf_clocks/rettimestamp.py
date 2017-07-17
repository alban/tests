#!/usr/bin/env python
# Copyright (c) PLUMgrid, Inc.
# Licensed under the Apache License, Version 2.0 (the "License")

from bcc import BPF

bpf_text="""
#include <uapi/linux/ptrace.h>

#ifndef ENOTDIR
#define ENOTDIR 20
#endif

int kretprobe__sys_newlstat(struct pt_regs *ctx) {
    int ret = (int) PT_REGS_RC(ctx);
    if (ret == -ENOTDIR)
        bpf_trace_printk("lstat! %llu ; (kretprobe) err=%d\\n", bpf_ktime_get_ns(), ret);
    return 0;
}
"""

BPF(text=bpf_text).trace_print()
