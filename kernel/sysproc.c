#include "types.h"
#include "riscv.h"
#include "defs.h"
#include "date.h"
#include "param.h"
#include "memlayout.h"
#include "spinlock.h"
#include "proc.h"


uint64 sys_exit(void) {
  int n;
  if (argint(0, &n) < 0) return -1;
  exit(n);
  return 0;  // not reached
}

uint64 sys_getpid(void) { return myproc()->pid; }

uint64 sys_fork(void) { return fork(); }

uint64 sys_wait(void) {
  uint64 p;
  int flags;
  if (argaddr(0, &p) < 0) return -1;
  if (argint(1, &flags) < 0) return -1;
  return wait(p, flags);
}

uint64 sys_sbrk(void) {
  int addr;
  int n;

  if (argint(0, &n) < 0) return -1;
  addr = myproc()->sz;
  if (growproc(n) < 0) return -1;
  return addr;
}

uint64 sys_sleep(void) {
  int n;
  uint ticks0;

  if (argint(0, &n) < 0) return -1;
  acquire(&tickslock);
  ticks0 = ticks;
  while (ticks - ticks0 < n) {
    if (myproc()->killed) {
      release(&tickslock);
      return -1;
    }
    sleep(&ticks, &tickslock);
  }
  release(&tickslock);
  return 0;
}

uint64 sys_kill(void) {
  int pid;

  if (argint(0, &pid) < 0) return -1;
  return kill(pid);
}

// return how many clock tick interrupts have occurred
// since start.
uint64 sys_uptime(void) {
  uint xticks;

  acquire(&tickslock);
  xticks = ticks;
  release(&tickslock);
  return xticks;
}

uint64 sys_rename(void) {
  char name[16];
  int len = argstr(0, name, MAXPATH);
  if (len < 0) {
    return -1;
  }
  struct proc *p = myproc();
  memmove(p->name, name, len);
  p->name[len] = '\0';
  return 0;
}

uint64 sys_yield(void) {
    struct proc *p = myproc();

    // ① 打印 context 的地址范围（左闭右开 [start, end)）
    exit_info("Save the context of the process to the memory region from address %p to %p\n", &p->context, &p->context + 1);

    // ② 打印当前进程的 pid 与用户态 pc
    exit_info("Current running process pid is %d and user pc is %p\n", p->pid, p->trapframe->epc);

    // ③ 从当前进程开始「环形」扫描，找下一个 RUNNABLE（只观察，不改变任何状态！）
    int    next_pid = 0;
    uint64 next_pc  = 0;
    for (int i = 0; i < NPROC; i++) {
        struct proc *q = &proc[(p - proc + i) % NPROC];   // 环形：i=0 是自己，绕一圈
        if (q == p) continue;                             // 自己是 RUNNING，跳过
        acquire(&q->lock);                                // state/pid 必须持锁读
        if (q->state == RUNNABLE) {
            next_pid = q->pid;
            next_pc  = q->trapframe->epc;
            release(&q->lock);                              // ★ 先放锁，再跳出
            break;
        }
        release(&q->lock);                                // ★ 每轮都配对释放
    }

    if (next_pid) {
        exit_info("Next runnable process pid is %d and user pc is %p\n", next_pid, next_pc);
    }

    // ④ 真正让出 CPU —— 这一句才叫 yield
    yield();
    return 0;
}
