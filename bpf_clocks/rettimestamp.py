#!/usr/bin/env python
# Copyright (c) PLUMgrid, Inc.
# Licensed under the Apache License, Version 2.0 (the "License")

from bcc import BPF

bpf_text="""
#include <uapi/linux/ptrace.h>

int kretprobe__sys_newlstat(struct pt_regs *ctx) {
    int ret = (int) PT_REGS_RC(ctx);
    if (ret == 0)
        bpf_trace_printk("lstat! %llu ; (kretprobe)\\n", bpf_ktime_get_ns());
    return 0;
}
"""

BPF(text=bpf_text).trace_print()
