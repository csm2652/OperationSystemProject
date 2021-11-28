#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "myscheduler.h"
struct {
  struct spinlock lock;
  struct proc proc[NPROC];
} ptable;

static struct proc *initproc;

struct spinlock threadlock;

int nextpid = 1;
extern void forkret(void);
extern void trapret(void);

static void wakeup1(void *chan);

extern struct stride stride_arr[NPROC];
extern struct FQ MLFQ_queues[3];


void
pinit(void)
{
  initlock(&ptable.lock, "ptable");
}

// Must be called with interrupts disabled
int
cpuid() {
  return mycpu()-cpus;
}

// Must be called with interrupts disabled to avoid the caller being
// rescheduled between reading lapicid and running through the loop.
struct cpu*
mycpu(void)
{
  int apicid, i;
  
  if(readeflags()&FL_IF)
    panic("mycpu called with interrupts enabled\n");
  
  apicid = lapicid();
  // APIC IDs are not guaranteed to be contiguous. Maybe we should have
  // a reverse map, or reserve a register to store &cpus[i].
  for (i = 0; i < ncpu; ++i) {
    if (cpus[i].apicid == apicid)
      return &cpus[i];
  }
  panic("unknown apicid\n");
}

// Disable interrupts so that we are not rescheduled
// while reading proc from the cpu structure
struct proc*
myproc(void) {
  struct cpu *c;
  struct proc *p;
  pushcli();
  c = mycpu();
  p = c->proc;
  popcli();
  return p;
}

//PAGEBREAK: 32
// Look in the process table for an UNUSED proc.
// If found, change state to EMBRYO and initialize
// state required to run in the kernel.
// Otherwise return 0.
static struct proc*
allocproc(void)
{
  struct proc *p;
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  //when initialize proc is finished, push proc to MLFQ and set mystride
  push_MLFQ(p, 0);
  p->mystride = stride_arr;
  return p;
}

//thread functions
static struct proc*
allocthread(struct proc *master)
{
  struct proc *p; 
  char *sp;

  acquire(&ptable.lock);

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == UNUSED)
      goto found;

  release(&ptable.lock);
  return 0;

found:
  p->state = EMBRYO;
  p->pid = nextpid++;

  release(&ptable.lock);

  // Allocate kernel stack.
  if((p->kstack = kalloc()) == 0){
    p->state = UNUSED;
    return 0;
  }
  sp = p->kstack + KSTACKSIZE;

  // Leave room for trap frame.
  sp -= sizeof *p->tf;
  p->tf = (struct trapframe*)sp;

  // Set up new context to start executing at forkret,
  // which returns to trapret.
  sp -= 4;
  *(uint*)sp = (uint)trapret;

  sp -= sizeof *p->context;
  p->context = (struct context*)sp;
  memset(p->context, 0, sizeof *p->context);
  p->context->eip = (uint)forkret;

  //thread master setting
  p->parent = master->parent;
  p->master = master;
  p->pgdir = master->pgdir;
  p->isthread = 1;
  master->isthread = 1;

  //thread mlfq setting
  p->fqtick = master->fqtick;
  p->level = master->level;
  p->mystride = master->mystride;
  p->isstride = master->isstride;

  int i;

  for(i=0; i < NTHRE; i++){
    if(master->workerthread[i]==0){
        master->workerthread[i] = p;
        master->threadcnt++;
        p->tindex = i;
        break;
    }
  }
  if(i == NTHRE){
    panic("no more threading");
  }

  return p;
}

int
allocusrstack(struct proc *master, struct proc *new)
{
    
    acquire(&threadlock);
    int i;

    //alloc usr stack where it's bean used and returned,
    for(i =0;i < NTHRE; i++){
        if(master->freeusrspace[i] != 0){
            if((new->sz = allocuvm(master->pgdir, master->freeusrspace[i], master->freeusrspace[i] + 2*PGSIZE)) == 0){
                release(&threadlock);
                return -1;
            }
            master->freeusrspace[i] = 0;
            goto guard;
        }
    }


    if((new->sz = allocuvm(master->pgdir, master->sz, master->sz + 2*PGSIZE))==0){
        release(&threadlock);
        return -1;
    }
    master->sz = new->sz;
guard:
    //set gaurdpage
    clearpteu(master->pgdir, (char*)(new->sz - 2*PGSIZE));
    release(&threadlock);
    return 0;
}

int
deallocstack(struct proc *worker){
    acquire(&threadlock);
    kfree(worker->kstack);

    int i;
    for(i=0; i< NTHRE;i++){
        if(worker->master->freeusrspace[i] == 0){
            if((worker->master->freeusrspace[i] = deallocuvm(worker->pgdir, worker->sz, worker->sz - 2*PGSIZE))==0){
                release(&threadlock);
                return -1;
            }
            else{
                release(&threadlock);
                return 0;
            }
        }
    }
    release(&threadlock);
    cprintf("\n nothing to dealloc stack");
    return -1;
}

int
thread_create(thread_t *thread, void *(*start_routine)(void*), void *arg){
    struct proc* p = myproc();
    struct proc* thrd;
    struct proc* master;
    int i;
    if(p->master == 0){
        master = p;
    }else{
        master = p->master;
    }

    uint ustack[3];
    uint sp;

    if((thrd = allocthread(master)) == 0){
        cprintf("allocthread prb");
        return -1;
    }
    if(allocusrstack(master, thrd) < 0){
        cprintf("allocstack prb");
        return -1;
    }

    //bring it from fork initialize file I/O
    for(i = 0; i < NOFILE; i++)
        if(master->ofile[i])
            thrd->ofile[i] = filedup(master->ofile[i]);
    thrd->cwd = idup(master->cwd);
    //set trapframe, copy from master and change sp, eip(point start_routine)
    safestrcpy(thrd->name, master->name, sizeof(master->name));

    *thrd->tf = *master->tf;
    thrd->tf->eip = (uint)start_routine;
    sp = thrd->sz;
    
    //for save arg for start routine
    ustack[0] = 0xffffffff;
    ustack[1] = (uint)arg;
    ustack[2] = 0;
 
    sp -= sizeof(ustack);

    if(copyout(master->pgdir, sp, ustack, sizeof(ustack)) < 0)
        return -1;
        
    thrd->tf->esp = sp;
    *thread = thrd->pid;
    
    acquire(&ptable.lock);
    thrd->state = RUNNABLE;
    release(&ptable.lock);
    return 0;
}

int
thread_join(thread_t thread, void **retval){

    struct proc* p;

    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->pid == thread)
            break;
        else if(p == &ptable.proc[NPROC])
            panic("no pid");
    }

    struct proc* join = p;
    //already done
    if(p->state == UNUSED){
        release(&ptable.lock);
        return 0;
    }
    while(join->state != ZOMBIE)
        sleep(p->master, &ptable.lock);

    if(deallocstack(join) == -1){
        release(&ptable.lock);
        return -1;
    }

    if(retval != 0){
        *retval = p->master->retvals[join->tindex];
    }
    p->master->workerthread[p->tindex] = 0;
    p->state = UNUSED;
    p->kstack = 0;

    p->killed = 0;
    release(&ptable.lock);
    return 0;
}

void thread_exit(void *retval)
{
    struct proc *curproc = myproc();
    struct proc *p;
    int fd;

    if(curproc->master ==0){
        exit();
    }

    for(fd = 0; fd < NOFILE; fd++){
        if(curproc->ofile[fd]){
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd]=0;
        }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;

    acquire(&ptable.lock);

    wakeup1(curproc->master);

    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
        if(p->parent == curproc){
            p->parent = initproc;
            if(p->state == ZOMBIE)
                wakeup1(initproc);
        }
    }
    curproc->master->retvals[curproc->tindex] = retval;
    curproc->state = ZOMBIE;
    curproc->master->threadcnt--;
    sched();
    panic("there is zombie thread!");
}       
/*
//thread RR controlled by master thread
struct proc*
pickthread(struct proc* master){
    struct proc* thread;
    struct proc* next;
    int recent,i;

    if(master->threadcnt == 0)
        return master;

    i = master->curthread;

    recent = master->curthread % NTHRE;
    thread = master->workerthread[recent];
    //find next runnable thread
    for(i= 1; i < NTHRE; i++){
        next = master->workerthread[(i+recent)%NTHRE];
        if((next != 0) && (next->state == RUNNABLE)){
            master->curthread = next->tindex;
            //cprintf("\nfind it: %d",next->tindex);
            return thread;
        }
        else
            continue;
    }
    if(i==NTHRE)
        return master;


    panic("\n mush eixt before here");
}
*/

//PAGEBREAK: 32
// Set up first user process.
void
userinit(void)
{
  struct proc *p;
  extern char _binary_initcode_start[], _binary_initcode_size[];

  // initialize stride_arr and MLFQ
  struct stride* strd = stride_arr;
  int i;
  int j;
  int l;
  //stride_arr[0] mean time to run MLFQ, so initialized with share 20%
  strd[0].stride = MAXTICKET / 20;
  strd[0].share = 20;
  strd[0].pass = 0;
  strd[0].proc = 0;
 
  for(i = 0; i<NPROC; i++){
      strd[i].stride = 0;
      strd[i].proc = 0;
  }

  for(j=0;j<3;j++){
      MLFQ_queues[j].total = 0;
      MLFQ_queues[j].current = 0;

      for(l=0; l <NPROC; l++){ 
          MLFQ_queues[j].holdingproc[l] = 0;
      }
  }

  
  p = allocproc();
  stride_arr[0].proc = p; 
  initproc = p;
 
  if((p->pgdir = setupkvm()) == 0)
    panic("userinit: out of memory?");
  inituvm(p->pgdir, _binary_initcode_start, (int)_binary_initcode_size);
  p->sz = PGSIZE;
  memset(p->tf, 0, sizeof(*p->tf));
  p->tf->cs = (SEG_UCODE << 3) | DPL_USER;
  p->tf->ds = (SEG_UDATA << 3) | DPL_USER;
  p->tf->es = p->tf->ds;
  p->tf->ss = p->tf->ds;
  p->tf->eflags = FL_IF;
  p->tf->esp = PGSIZE;
  p->tf->eip = 0;  // beginning of initcode.S

  safestrcpy(p->name, "initcode", sizeof(p->name));
  p->cwd = namei("/");

  // this assignment to p->state lets other cores
  // run this process. the acquire forces the above
  // writes to be visible, and the lock is also needed
  // because the assignment might not be atomic.
  acquire(&ptable.lock);

  p->state = RUNNABLE;
  release(&ptable.lock);
  
}

// Grow current process's memory by n bytes.
// Return 0 on success, -1 on failure.
int
growproc(int n)
{
  uint sz;
  struct proc *curproc = myproc();
  struct proc *master;

  master = curproc;
  if(curproc->master != 0){
    master = curproc->master;
  }
  sz = master->sz;
  if(n > 0){
    if((sz = allocuvm(master->pgdir, sz, sz + n)) == 0)
      return -1;
  } else if(n < 0){
    if((sz = deallocuvm(master->pgdir, sz, sz + n)) == 0)
      return -1;
  }
  master->sz = sz;
  switchuvm(curproc);
  return 0;
}

// Create a new process copying p as the parent.
// Sets up stack to return as if from system call.
// Caller must set state of returned proc to RUNNABLE.
int
fork(void)
{
  int i, pid;
  struct proc *np;
  struct proc *curproc = myproc();

  // Allocate process.
  if((np = allocproc()) == 0){
    return -1;
  }

  // Copy process state from proc.
  if((np->pgdir = copyuvm(curproc->pgdir, curproc->sz)) == 0){
    kfree(np->kstack);
    np->kstack = 0;
    np->state = UNUSED;
    return -1;
  }
  np->sz = curproc->sz;
  np->parent = curproc;
  *np->tf = *curproc->tf;

  // Clear %eax so that fork returns 0 in the child.
  np->tf->eax = 0;

  for(i = 0; i < NOFILE; i++)
    if(curproc->ofile[i])
      np->ofile[i] = filedup(curproc->ofile[i]);
  np->cwd = idup(curproc->cwd);

  safestrcpy(np->name, curproc->name, sizeof(curproc->name));

  pid = np->pid;

  acquire(&ptable.lock);

  np->state = RUNNABLE;

  release(&ptable.lock);

  return pid;
}

// Exit the current process.  Does not return.
// An exited process remains in the zombie state
// until its parent calls wait() to find out it exited.
void
exit(void)
{
  struct proc *curproc = myproc();
  struct proc *p;
  struct proc *master;
  int fd;

  if(curproc->master ==0){
    master = curproc;
  }else{
    master = curproc->master;
  }

  if(curproc == initproc)
    panic("init exiting");

  int i;

  //clean up including thread
  for(i = 0; i< NTHRE; i++){
    if(master->workerthread[i]==0){
        continue;
    }else{
        curproc = master->workerthread[i];
    }

    if(curproc == initproc)
        panic("init exiting");

    for(fd = 0; fd < NOFILE; fd++){
        if(curproc->ofile[fd]){
            fileclose(curproc->ofile[fd]);
            curproc->ofile[fd] = 0;
        }
    }

    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;
  }

  //for master thread;
  curproc = master;
  for(fd = 0; fd < NOFILE; fd++){
    if(curproc->ofile[fd]){
        fileclose(curproc->ofile[fd]);
        curproc->ofile[fd]=0;
    }
  }
  if(curproc->cwd){
    begin_op();
    iput(curproc->cwd);
    end_op();
    curproc->cwd = 0;
  }

  acquire(&ptable.lock);

  // Parent might be sleeping in wait().
  wakeup1(curproc->parent);

  // Pass abandoned children to init.
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->parent == curproc){
      p->parent = initproc;
      if(p->state == ZOMBIE)
        wakeup1(initproc);
    }
  }
  for(i =0; i<NTHRE; i++){
    if(master->workerthread[i] != 0){
        master->workerthread[i]->state = ZOMBIE;
    }
  }
  //pop mlfq or clear stride element
  if(master->isstride == 1){
    // give editional ticket to mlfq when stride proc finish
    stride_arr[0].share = stride_arr[0].share + master->mystride->share;
    stride_arr[0].stride = MAXTICKET/ stride_arr[0].share;

    master->mystride->stride = 0;
    master->mystride->pass = 0;
    master->mystride->share = 0;

  }
  else{
      pop_MLFQ(curproc);
  }


  // Jump into the scheduler, never to return.
  curproc->state = ZOMBIE;
  sched();
  panic("zombie exit");
}

// Wait for a child process to exit and return its pid.
// Return -1 if this process has no children.
int
wait(void)
{
  struct proc *p;
  int havekids, pid, i;
  struct proc *curproc = myproc();
 
  acquire(&ptable.lock);
  for(;;){
    // Scan through table looking for exited children.
    havekids = 0;
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->parent != curproc || p->master != 0)
        continue;
      havekids = 1;
      if(p->state == ZOMBIE){
        for(i=0; i < NTHRE; i++){
            if(p->workerthread[i] != 0){
                kfree(p->workerthread[i]->kstack);
                p->workerthread[i]->kstack = 0;
                p->workerthread[i]->parent = 0;
                p->workerthread[i]->name[0] = 0;
                p->workerthread[i]->pid = 0;
                p->workerthread[i]->state = UNUSED;
                p->workerthread[i]->killed = 0;
                p->workerthread[i] = 0;
            }
        }
        // Found one.
        pid = p->pid;
        kfree(p->kstack);
        p->kstack = 0;
        freevm(p->pgdir);
        p->pid = 0;
        p->parent = 0;
        p->name[0] = 0;
        p->killed = 0;
        p->state = UNUSED;
        release(&ptable.lock);
        return pid;
      }
    }

    // No point waiting if we don't have any children.
    if(!havekids || curproc->killed){
      release(&ptable.lock);
      return -1;
    }

    // Wait for children to exit.  (See wakeup1 call in proc_exit.)
    sleep(curproc, &ptable.lock);  //DOC: wait-sleep
  }
}

//PAGEBREAK: 42
// Per-CPU process scheduler.
// Each CPU calls scheduler() after setting itself up.
// Scheduler never returns.  It loops, doing:
//  - choose a process to run
//  - swtch to start running that process
//  - eventually that process transfers control
//      via swtch back to the scheduler.
void
scheduler(void)
{
  struct proc *p;
  struct proc *chosen;
  struct cpu *c = mycpu();
  c->proc = 0;
  for(;;){
    // Enable interrupts on this processor.
    sti();
    // Loop over process table looking for process to run.
    acquire(&ptable.lock);
    for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
      if(p->state != RUNNABLE)
          continue;
     
      chosen = choose_lowpass(0);
      //it mean nothing in mlfq
      if(chosen == 0){
          //make not to stride_arr[0](mlfq) occupy all of cpu!
          stride_arr[0].pass += stride_arr[0].stride;
          chosen = choose_lowpass(1);
      }
     
      if(chosen ==0){
        chosen = p;
      }

      // Switch to chosen process.  It is the process's job
      // to release ptable.lock and then reacquire it
      // before jumping back to us.
      c->proc = chosen;
      switchuvm(chosen);
      chosen->state = RUNNING;

      swtch(&(c->scheduler), chosen->context);
      switchkvm();

      // Process is done running for now.
      // It should have changed its p->state before coming back.
      c->proc = 0;
    }
    release(&ptable.lock);

  }
}

// Enter scheduler.  Must hold only ptable.lock
// and have changed proc->state. Saves and restores
// intena because intena is a property of this
// kernel thread, not this CPU. It should
// be proc->intena and proc->ncli, but that would
// break in the few places where a lock is held but
// there's no process.
void
sched(void)
{
  int intena;
  struct proc *p = myproc();

  if(!holding(&ptable.lock))
    panic("sched ptable.lock");
  if(mycpu()->ncli != 1)
    panic("sched locks");
  if(p->state == RUNNING)
    panic("sched running");
  if(readeflags()&FL_IF)
    panic("sched interruptible");
  intena = mycpu()->intena;
  swtch(&p->context, mycpu()->scheduler);
  mycpu()->intena = intena;
}

// Give up the CPU for one scheduling round.
void
yield(void)
{
  acquire(&ptable.lock);  //DOC: yieldlock
  myproc()->state = RUNNABLE;
  sched();
  release(&ptable.lock);
}

// A fork child's very first scheduling by scheduler()
// will swtch here.  "Return" to user space.
void
forkret(void)
{
  static int first = 1;
  // Still holding ptable.lock from scheduler.
  release(&ptable.lock);

  if (first) {
    // Some initialization functions must be run in the context
    // of a regular process (e.g., they call sleep), and thus cannot
    // be run from main().
    first = 0;
    iinit(ROOTDEV);
    initlog(ROOTDEV);
  }

  // Return to "caller", actually trapret (see allocproc).
}

// Atomically release lock and sleep on chan.
// Reacquires lock when awakened.
void
sleep(void *chan, struct spinlock *lk)
{
  struct proc *p = myproc();
  
  if(p == 0)
    panic("sleep");

  if(lk == 0)
    panic("sleep without lk");

  // Must acquire ptable.lock in order to
  // change p->state and then call sched.
  // Once we hold ptable.lock, we can be
  // guaranteed that we won't miss any wakeup
  // (wakeup runs with ptable.lock locked),
  // so it's okay to release lk.
  if(lk != &ptable.lock){  //DOC: sleeplock0
    acquire(&ptable.lock);  //DOC: sleeplock1
    release(lk);
  }
  // Go to sleep.
  p->chan = chan;
  p->state = SLEEPING;

  sched();

  // Tidy up.
  p->chan = 0;

  // Reacquire original lock.
  if(lk != &ptable.lock){  //DOC: sleeplock2
    release(&ptable.lock);
    acquire(lk);
  }
}

//PAGEBREAK!
// Wake up all processes sleeping on chan.
// The ptable lock must be held.
static void
wakeup1(void *chan)
{
  struct proc *p;

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++)
    if(p->state == SLEEPING && p->chan == chan)
      p->state = RUNNABLE;
}

// Wake up all processes sleeping on chan.
void
wakeup(void *chan)
{
  acquire(&ptable.lock);
  wakeup1(chan);
  release(&ptable.lock);
}

// Kill the process with the given pid.
// Process won't exit until it returns
// to user space (see trap in trap.c).
int
kill(int pid)
{
  struct proc *p;

  acquire(&ptable.lock);
  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->pid == pid){
      p->killed = 1;
      // Wake process from sleep if necessary.
      if(p->state == SLEEPING)
        p->state = RUNNABLE;
      release(&ptable.lock);
      return 0;
    }
  }
  release(&ptable.lock);
  return -1;
}

//PAGEBREAK: 36
// Print a process listing to console.  For debugging.
// Runs when user types ^P on console.
// No lock to avoid wedging a stuck machine further.
void
procdump(void)
{
  static char *states[] = {
  [UNUSED]    "unused",
  [EMBRYO]    "embryo",
  [SLEEPING]  "sleep ",
  [RUNNABLE]  "runble",
  [RUNNING]   "run   ",
  [ZOMBIE]    "zombie"
  };
  int i;
  struct proc *p;
  char *state;
  uint pc[10];

  for(p = ptable.proc; p < &ptable.proc[NPROC]; p++){
    if(p->state == UNUSED)
      continue;
    if(p->state >= 0 && p->state < NELEM(states) && states[p->state])
      state = states[p->state];
    else
      state = "???";
    cprintf("%d %s %s", p->pid, state, p->name);
    if(p->state == SLEEPING){
      getcallerpcs((uint*)p->context->ebp+2, pc);
      for(i=0; i<10 && pc[i] != 0; i++)
        cprintf(" %p", pc[i]);
    }
    cprintf("\n");
  }
}

int
getppid(void)
{
    return myproc()->parent->pid;
}

