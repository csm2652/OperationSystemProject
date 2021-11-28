#include "types.h"
#include "defs.h"
#include "param.h"
#include "memlayout.h"
#include "mmu.h"
#include "x86.h"
#include "proc.h"
#include "spinlock.h"
#include "myscheduler.h"

//stride array, if set_cpu_share called, proc move mlfq to stride_arr
struct stride stride_arr[NPROC];

//mlfq_queues,0~2 level of priority
struct FQ MLFQ_queues[3];

//to check time allotment
int MLFQ_tick;

//tick lock to increase tick
struct spinlock FQ_ticklock;


extern struct{
    struct spinlock lock;
    struct proc proc[NPROC];
}ptable;

// push proc to feedback queue, level is same as array position
int
push_MLFQ(struct proc* p, int level){
    int i;
    for(i = 0; i<NPROC; i++){
        if(MLFQ_queues[level].holdingproc[i] == 0){
            MLFQ_queues[level].holdingproc[i] = p;
            p->level = level;
            p->fqtick = 0;
            p->mystride = stride_arr;
            p->isstride = 0;
            MLFQ_queues[level].total++;
            return 0;
        }
    }
    return -1;
}

//pop proc from FQ
int
pop_MLFQ(struct proc* p){

    int level = p->level;
    int i;
    for(i =0; i<NPROC; i++){
        if(MLFQ_queues[level].holdingproc[i] == p){
            p->fqtick = 0;
            MLFQ_queues[level].total--;
            MLFQ_queues[level].holdingproc[i] = 0;
            return 0;
        
        }
    }
    //nothing match: error
    return -1;

}

//find proc to exec
struct proc*
exec_MLFQ(void)
{
    int i;
    int j;
    int pos;
    for(i = 0; i < 3; i++){
        if(MLFQ_queues[i].total == 0){
            continue;
        }
        pos = MLFQ_queues[i].current;// recent using position which can store proc
        
        pos= pos%NPROC;
        for(j = 0; j < NPROC-1; j++){
            //find first exec proc
            if(MLFQ_queues[i].holdingproc[pos] != 0 
                    && MLFQ_queues[i].holdingproc[pos]->state == RUNNABLE){
              MLFQ_queues[i].current = pos;
              MLFQ_queues[i].current++;
              //if mlfq proc is master thread
              if(MLFQ_queues[i].holdingproc[pos]->isthread == 1)
                  return pickthread(MLFQ_queues[i].holdingproc[pos]);

              //return mlfq proc
              return MLFQ_queues[i].holdingproc[pos];
            }
            else{
                //if pos > 64, error so %NPROC
                pos = (pos + 1) % NPROC;
            }
      
        }
    }
    return 0;
}

int level_change_MLFQ(struct proc* p, int level){
    int ret = pop_MLFQ(p);
    if(ret == -1)//there is no proc p in queue
        return ret;
    return push_MLFQ(p, level);
}

//call when yield, timer interupt called
int tick_add_MLFQ(void){

    acquire(&ptable.lock);
    struct proc* p = myproc();
    struct stride* now = myproc()->mystride;
    // add stride to pass
    now->pass = now->pass + now->stride;
    
    if(p->isstride == 1){
        increasealltick(p);
        if(p->fqtick > 5){
            release(&ptable.lock);
            return 1;
        }
        else{
            release(&ptable.lock);
            return 0;
        }
    }

    acquire(&FQ_ticklock);
    //if MLFQ_tick over 100 ? -> boost priority
    MLFQ_tick++;
    release(&FQ_ticklock);
    
    increasealltick(p);
    int allot = p->fqtick;

    if(p->level == 0){
        if(allot >= 20){
            level_change_MLFQ(p, 1);
        }
        if((allot)%5 == 0){
            if(MLFQ_tick >200){
                boost_priority();
            }
            release(&ptable.lock);
            return 1;
        }else{
        release(&ptable.lock);
        return 0;
        }
    }
    else if(p->level == 1){
        if(allot >= 40){
            level_change_MLFQ(p, 2);
        }
        //first time when proc start in level 1 allot must 6,
        //so apply 1 to keep  2 tick time allotment
        if((allot)%10 == 0){
            if(MLFQ_tick >200){
                boost_priority();
            }
            release(&ptable.lock);
            return 1;
        }else{
            release(&ptable.lock);
            return 0;
        }
    }
    else if(p->level == 2){
        if((allot)%20 == 0){
            if(MLFQ_tick >200){
                boost_priority();
            }
            release(&ptable.lock);
            return 1;
        }else{
            release(&ptable.lock);
            return 0;
        }
    }
    release(&ptable.lock);
    return -1;

}



void
boost_priority(void){
    struct proc* p = ptable.proc;
    int i;
    for(i = 0; i < NPROC; i++){
        if(p[i].level == 1 || p[i].level == 2){
            level_change_MLFQ(&p[i], 0);
        }
    }
    acquire(&FQ_ticklock);
    MLFQ_tick = 0;
    release(&FQ_ticklock);
}

//find lowest pass
struct proc*
choose_lowpass(int start){
    struct stride* select = stride_arr;
    struct stride* tmp;
    struct proc* return_proc;
    int i;
        
    for(tmp = stride_arr; tmp < &stride_arr[NPROC]; tmp++){
        if(tmp->stride == 0 || tmp->proc->state != RUNNABLE){
            continue;
        }
        
        if(tmp->pass < select->pass){
            select = tmp;
        }
    }
    //if select pass is over 200000000, pass can overflow 
    if(select->pass > 200000000){
         for(i = 0; i < NPROC; i++){
            tmp[i].pass = tmp[i].pass % 200000000;
         }
    }

    //when mlfq select
    if (select == stride_arr){
        return_proc = exec_MLFQ();
        // when nothing inside mlfq return 0
        if(return_proc == 0){
            return 0;
        }
        return return_proc;
        
    }

    //when stride proc is masterthread
    if(select->proc->isthread == 1){
        return pickthread(select->proc);
    }

    return select->proc;
        
}

//thread RR controlled by master thread
/*struct proc*
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
}*/

struct proc*
pickthread(struct proc* master){

	if(master->threadcnt == 0)
		return master;
	int i = master->curthread;
	do{
		i = (i + 1) % (NTHRE + 1);
		if(i == NTHRE){
			if(master->state == RUNNABLE){
				master->curthread = i;
				return master;
			}else{
				continue;
			}
		}
		if(master->workerthread[i] != 0 && master->workerthread[i]->state == RUNNABLE){
			master->curthread = i;
			return master->workerthread[i];
		}
	}while(i != master->curthread);
	
	return master;
}


int
set_cpu_share(int portion){
    struct stride* select = stride_arr;
    int min_pass = 220000000;//overflow check
    int i;
    int j;
    int l;
    int total_portion;
    struct proc* p = myproc();

    //total portion except mlfq portion
    total_portion = portion;
    for(i=1; i < NPROC; i++){
        if(select[i].stride != 0){
            total_portion = total_portion + select[i].share;
        }
    }
    //always under 80
    if(total_portion > 80){
        return -1;
    }
    //if total use of cpu is under 100% give MLFQ extra cpu to run
    stride_arr[0].share = 100 - total_portion;
    stride_arr[0].stride = MAXTICKET / stride_arr[0].share;

    for(l=0; l < NPROC; l++){
        if(select[l].stride != 0)
            if(min_pass > select[l].pass){
                min_pass = select[l].pass;
            }
    }

    for(j = 0;j< NPROC; j++){

        if(select[j].stride ==0){
            select[j].share = portion;
            select[j].stride = MAXTICKET / portion;
            //this code make not to new proc occupy all cpu.
            select[j].pass = min_pass;
            break;
        }
    }


    select[j].proc = p;
    p->mystride = &select[j];
    p->isstride = 1;
    //set all relative thread fqtick = 0;
    setthreadtick(p);
    
    pop_MLFQ(p);
    p->level = 3;// for getlev(), if stride proc show level 3
    return 0;    

}
//make master and workerthread fqtick = 0, reuse it for check stride time quantum
void
setthreadtick(struct proc* thread){
    struct proc* master;
    int i;
    if(thread->master == 0)
        master = thread;
    else
        master = thread->master;

    master->fqtick = 0;

    if(master->isthread == 1){
        for(i=0; i< NTHRE; i++){
            master->workerthread[i]->fqtick = 0;
        }
    }
    
}
void
increasealltick(struct proc* p){
    struct proc* master;
    int i;

    if(p->isthread == 0){
        p->fqtick++;
    }
    else{
        p->fqtick++;
        for(i=0; i< NTHRE; i++){
            master->workerthread[i]->fqtick++;
        }
    }
    

}

int
getlev(){
    return 0;
}
