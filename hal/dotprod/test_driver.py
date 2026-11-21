#!/usr/bin/env python3
"""Compile actual driver functions against a small, single-threaded MMIO model."""
from pathlib import Path
import os
import subprocess
import tempfile

here = Path(__file__).resolve().parent
source = (here / 'fpga_drv.c').read_text()

def function(name):
    start = source.rfind('static ', 0, source.index(name + '('))
    opening = source.index('{', start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]

macros = source[source.index('#define MAP_BYTES'):source.index('/* Commands are matched')]
prelude = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <signal.h>
#include <sys/types.h>
typedef uint32_t u32;
typedef uint8_t u8;
#ifdef __APPLE__
typedef long long loff_t;
#endif
typedef int irqreturn_t;
#define __user
#define BIT(n) (1U << (n))
#define IRQ_NONE 0
#define IRQ_HANDLED 1
#define POLL_IN 1
#define IS_ERR(p) (!(p))
#define PTR_ERR(p) (-ENOMEM)
#define mutex_lock(p) ((void)0)
#define mutex_unlock(p) ((void)0)
#define spin_lock(p) ((void)0)
#define spin_unlock(p) ((void)0)
#define spin_lock_irqsave(p,f) ((f)=0)
#define spin_unlock_irqrestore(p,f) ((void)(f))
#define get_user(v,p) ((v)=*(p),0)
#define put_user(v,p) (*(p)=(v),0)
#define kfree(p) free(p)
struct file { int unused; };
struct {
    unsigned char *base;
    unsigned long status, result, num_elements;
    bool pending;
    int data_mutex, state_lock, fasync_fpga_queue;
} l;
static int interruptcount, signals, copy_fail;
static unsigned char mmio[0x40000];
static unsigned writes;
static size_t mmio_offset(const void *p) {
    size_t offset = (const unsigned char *)p - mmio;
    assert(offset % 4 == 0 && offset <= sizeof(mmio)-4);
    return offset;
}
static u32 peek(unsigned offset) {
    u32 v; memcpy(&v, mmio+offset, 4); return v;
}
static void poke(unsigned offset, u32 v) { memcpy(mmio+offset, &v, 4); }
static u32 readl(const void *p) {
    size_t offset = mmio_offset(p);
    u32 v = peek(offset);
    if (offset == 0) poke(0, v & ~(BIT(1)|BIT(3)));
    return v;
}
static void writel(u32 v, void *p) {
    size_t offset = mmio_offset(p);
    if (offset == 0x0c) v ^= peek(offset); /* TOW */
    poke(offset, v);
    ++writes;
}
static void *memdup_user(const void *p, size_t bytes) {
    void *copy = malloc(bytes); assert(copy); memcpy(copy,p,bytes); return copy;
}
static int copy_to_user(void *to, const void *from, size_t n) {
    if (copy_fail) return 1;
    memcpy(to,from,n); return 0;
}
static void kill_fasync(void *p, int sig, int band) { ++signals; }
'''
tests = r'''
int main(void) {
    unsigned count, i;
    unsigned long n, status;
    struct { u32 result, status; } out;
    u8 *input = malloc(2*NUM_INPUT_BYTES);
    assert(input);
    l.base = mmio;
    poke(0, CONTROL_IDLE);
    assert(!fpga_busy());
    poke(0, CONTROL_IDLE|CONTROL_START); assert(fpga_busy());
    poke(0, 0); assert(fpga_busy());
    poke(0, CONTROL_IDLE);

    writes=0; fpga_ack_irq(); assert(writes==0);
    poke(0x0c,3); fpga_ack_irq(); assert(peek(0x0c)==0);
    fpga_ack_irq(); assert(peek(0x0c)==0); /* no phantom toggle */
    assert(fpga_int_handler(0,NULL)==IRQ_NONE);

    /* Every supported size, including final two-byte tails. */
    for (count=1; count<=1024; ++count) {
        size_t bytes=count*34;
        for (i=0;i<2*bytes;++i) input[i]=(u8)(i*37+count);
        memset(mmio+X_MEM_OFFSET,0xa5,NUM_INPUT_BYTES);
        memset(mmio+Y_MEM_OFFSET,0xa5,NUM_INPUT_BYTES);
        assert(fpga_write1(NULL,(char *)input,2*bytes,NULL)==(ssize_t)(2*bytes));
        assert(!memcmp(mmio+X_MEM_OFFSET,input,bytes));
        assert(!memcmp(mmio+Y_MEM_OFFSET,input+bytes,bytes));
        if (bytes%4) {
            assert(mmio[X_MEM_OFFSET+bytes]==0 && mmio[X_MEM_OFFSET+bytes+1]==0);
            assert(mmio[Y_MEM_OFFSET+bytes]==0 && mmio[Y_MEM_OFFSET+bytes+1]==0);
        }
    }
    assert(fpga_write1(NULL,(char *)input,0,NULL)==-EINVAL);
    assert(fpga_write1(NULL,(char *)input,69,NULL)==-EINVAL);
    n=32; /* mismatch with uploaded 32768 elements */
    assert(fpga_ioctl1(NULL,CMD_START_ACC,(unsigned long)&n)==-EINVAL);
    n=32768;
    assert(fpga_ioctl1(NULL,CMD_START_ACC,(unsigned long)&n)==0);
    assert(peek(REG_LENGTH_OFFSET)==32768 && peek(0)==CONTROL_START);
    assert(l.pending);
    assert(fpga_ioctl1(NULL,CMD_START_ACC,(unsigned long)&n)==-EBUSY);
    assert(fpga_write1(NULL,(char *)input,68,NULL)==-EBUSY);
    assert(fpga_ioctl1(NULL,CMD_RESET_ACC,0)==-EBUSY);
    assert(fpga_read1(NULL,(char *)&out,8,NULL)==-EAGAIN);
    assert(fpga_ioctl1(NULL,CMD_CHECK_STATUS,(unsigned long)&status)==0 && status==STATUS_BUSY);

    poke(0,CONTROL_IDLE|BIT(1));
    poke(REG_RESULT_OFFSET,0x3fc00000); poke(REG_STATUS_OFFSET,STATUS_DONE);
    poke(REG_ISR_OFFSET,IRQ_DONE);
    (void)fpga_busy(); /* clears AP_CTRL.done, but must not lose ISR */
    assert(fpga_int_handler(0,NULL)==IRQ_HANDLED);
    assert(peek(REG_ISR_OFFSET)==0 && interruptcount==1 && signals==1);
    assert(fpga_int_handler(0,NULL)==IRQ_NONE);
    assert(fpga_ioctl1(NULL,CMD_CHECK_STATUS,(unsigned long)&status)==0 && status==STATUS_DONE);
    copy_fail=1;
    assert(fpga_read1(NULL,(char *)&out,8,NULL)==-EFAULT && l.pending);
    copy_fail=0;
    assert(fpga_read1(NULL,(char *)&out,8,NULL)==8 && !l.pending);
    assert(out.result==0x3fc00000 && out.status==STATUS_DONE);
    assert(fpga_read1(NULL,(char *)&out,8,NULL)==-EAGAIN);
    assert(fpga_ioctl1(NULL,CMD_START_ACC,(unsigned long)&n)==0);
    poke(0,CONTROL_IDLE); poke(REG_STATUS_OFFSET,6); poke(REG_ISR_OFFSET,1);
    assert(fpga_int_handler(0,NULL)==IRQ_HANDLED);
    assert(fpga_read1(NULL,(char *)&out,8,NULL)==8 && out.status==6);
    assert(fpga_ioctl1(NULL,CMD_RESET_ACC,0)==0);
    assert(peek(REG_ISR_OFFSET)==0 && !l.pending && !l.status);
    free(input);
    puts("PASS: all 1024 transfer sizes, padding, TOW IRQs, clear-on-read isolation, busy/start/reset, completion and copy retry");
}
'''
names = ['fpga_busy', 'fpga_ack_irq', 'fpga_upload', 'fpga_int_handler',
         'fpga_write1', 'fpga_read1', 'fpga_ioctl1']
header = (here / '../hls/impl/xdotprod_accel_hw.h').resolve()
code = prelude + '\n#include "' + str(header) + '"\n' + macros
code += '\n'.join(function(name) for name in names) + tests
with tempfile.TemporaryDirectory(prefix='dotprod-driver-') as tmp:
    c = Path(tmp)/'test.c'
    exe = Path(tmp)/'test'
    c.write_text(code)
    subprocess.run([os.environ.get('CC','cc'), '-std=gnu11', '-O1',
                    '-Wall', '-Wextra', '-Wno-unused-parameter',
                    '-fsanitize=undefined', '-fno-sanitize-recover=all',
                    str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
