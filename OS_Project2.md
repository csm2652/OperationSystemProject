#  The First Mileston: Design Basic LWP Operations



### Process / Thread

- 프로세스는 하나의 작업을 처리하는 기본 단위로, 실행에 필요한 자원(file descriptor, address space)을 각각 가지고 있으며  스케쥴러에 의해 일정 규칙에 따라 실행된다.

- 쓰레드는 프로세스를 다시 여러 파트로 나누어 빠르게 처리 할 수 있게 만든 light-weight process이다. 상위 프로세스를 공유하는 LWP끼리 자원을 공유하며,  이러한 기능을 통해 standard한 pthread는 병렬 처리를 지원한다. 

  

### Posix thread

+ int pthread_create(pthread_t *thread, const pthread_attr_t *attr, void \*(\*start_routine)(void *), void *arg)
  + 이 함수를 통해서 worker thread를 생성한다. worker thread는 4번째 인자인 arg를 3번째 인자인 start_routine의 인자로 사용하며, 실행시킨다. 또한, create 호출 후 join이 나올때까지 마스터 thread는 대기 상태가 된다.   
+ int pthread_join(pthread_t thread, void **ret_val)
  + 마스터 pthread가 create를 부른 직후 worker thread가 생성된다. 이때, worker thread가 작업을 끝날때까지 즉, pthread_exit를 부를때까지 join에서 기다린다. 
+ void pthread_exit(void *ret_val)
  + worker thread가 끝나면 이 함수를 호출하여 pthread_join 함수에서 기다리는 mater thread를 wake up 시키고, ret_val은 join의 ret_val로 들어간다.



### Design Basic LWP Operations for xv6



#### thread 디자인

> thread는 결국 process의 파생이다.

+ 기본적으로 thread 또한 process의 일환이므로 proc 구조체에 몇몇 프로퍼티를 추가해서 thread와 일반 process을 구분합니다.

  ```c
  struct proc{
  ///base property    
     ...
  /// add
    int isThread; 
    thread_t parent;// thread_t: unsigned int to show parent pid
    void* ret_val[NTHRE]://save return value here, NTHRE is number of maximum thread
  }
  ```

  

> thread들은 stack을 제외한 address space를 공유한다. 

+ thread_alloc에서 thread를 만든다. 이때  쓰레드는 thread_create를 호출한 부모 프로세스의 주소공간을 그대로 복사해 오되, stack page만 새로 할당 받은 뒤에 thread에 추가해준다.
+ 이를 통해 같은 proc thread 간 context switch에 시간이 적게 걸리게 된다.

>thread는 master와 worker로 나뉜다.

+ thread create로 worker  thread가 만들어지면 master는 sleep으로 기다리고 있다가 worker의 일이 끝나고  thread_exit가 발동되면 thread_join에서부터 다시 진행이 된다.
+ worker thread의 결과값( 리턴 값)은 ret_val에 저장되어 넘어 온다.

#### MLFQ and Stride Scheduling

> (MLFQ) 같은 thread 그룹끼리는 time quantum과 time alloment를 공유한다.

+ 하나의 Thread 당 작업은 1tick이므로 타임 인터럽트 한번 일어 날때마다 자신의 그룹의 총 time quantum과 allotment을 체크한 뒤 만약 시간을 다 사용했으면 다음 프로세스로 넘긴다.
+ 이때, 충분히 시간을 사용하지 못하였으면 작업 scheduler 큐 안에서 자신과 같은 그룹인 thread를 찾아 실행시킨다.

>(Stride) thread가 set_cpu_share를 부르면 해당 thread group의 cpu share를 보장한다.

+ set_cpu_share 시스템 콜이 들어오면 해당 thread의 그룹은 즉시 stride scheduler로 이동한다.
+ thread group(프로세스)는  stride scheduler로 관리되면서 스케쥴러에 인해 선택 받았을 시 5tick의 작업시간을 보장받으면 내부 쓰레드를 5tick 동안 진행시키게 된다.

#### Interaction with other service in xv6

> Basic Operation

+ 위에서 설명한 바와 같다. thread create가 호출되는 순간 부터 Thread로 돌아가는 proc임을 알리는 isThread를 1로 바꾸며 thread 정책에 따라 create, join, exit을 불러가며 작동하게된다.

> Exit

+ 모든 thread 소멸 시에 이 함수를 호출하여 memory 관리를 철저히 한다.

> Fork

+ 일반 proc에서 호출과 크게 다르지 않다. Thread에서 fork가 call 되면 해당 부모 프로세스를 기반으로 하는 single thread process가 만들어진다.

> Exec

+ exec으로 코드가 넘어가면 모든 작업이 exec이 가르킨 proc으로 완전히 넘어가서 안 돌아오기 때문에 모든 Thread를 확실하게 정리하는 것이 중요하다. 또한 exex을 통해 넘어간 proc이 확실히 실행 될 수 있도록 보장한다.
+ exec 함수에서 thread를 정리하는 함수를 추가할 것이다.

> Kill

+ 하나의 쓰레드라도 kill이 되면, 같은 thread group의 모든 thread들이 다 같이 종료되어야 한다.
+ 하나의 thread가 kill이 되면 같은 작업스케쥴러 큐에서 같은 그룹의 thread를 탐색에서 다 삭제해 줄 것이다.

> pipe

+ 한 thread가 다른 프로세스와 pipe로 연결이 되어 있다면, 같은 그룹내의 모든 thread들은 이를 공유할 수 있다.
+ 한 쓰레드가 pipe연결이 될 시 같은 그룹의 thread를 순회 하면서 pipe을 공유한다.

> sleep

+ 다른 syscall 과는 다르게 sleep은 sleep을 호출한 해당 thread만 해당 함수가 적용된다. 따라서 같은 그룹 내 나머지 thread들은 정상 작동한다.
+ 하지만, 이때 해당 process(부모)가 종료된다면 자고 있는 thread 또한 종료되어야만 한다.
+ 항상 프로세스 exit가 호출될 시 sleep thread를 확인한다.

> Other corner cases

+ 완성 후 추가적으로 고민이 필요

>  Interaction with the schedulers that you made in Project 1

+ 위에서 설명한 바와 같다.

### Detail of thread

가장 핵심 기능인 thread의 create, join, exit을 중점적으로 만들었다.

>  핵심 개요

+ 기본적으로 모든 프로세스는 처음 생성되었을때, 쓰레드가 아닌 일반 proc으로써 돌아가게 된다. 그러다 thread create가 불려지면, 그 순간 쓰레드가 된다. create를 호출한 쓰레드는 master 쓰레드가 되고 만들어진 쓰레드는 worker thread가 된다. 

  워커 쓰레드는 master thread에 의해 실행되며, 스케쥴러에 선택 받지 못한다.  즉, 스케쥴러에서 MLFQ 및 STRIDE 을 통해 master 쓰레드만을 선택하고 선택 된 것이process면 일반적인 방법으로 process를 돌리고, master 쓰레드가 선택되었으면, master thread이 가리키는 worker thread 목록에서 round robin이 추가적으로 작동 한 뒤 동작해야하는 thread를 반환한다.

> Thread Create

+ thread_create는 proc.c에 정의 되어있다. 

  alloc thread와 allocusrstack을 통해 만들어 지며 각각 kstack및 context 설정 userstack 설정을 맡는다. thread 또한 기본적으로 proc구조체의 형태로 이루어져 있으므로 allocthread는 allocproc과 거의 같은 방식으로 만들어지며 마스터의 pgdir을 가져오고, 각종 쓰레드 관련 세팅과 mlfq, stride위한 세팅을 한다.

  allcousrstack()은 master와 다른 usrstack을 할당하기 위해 별도로 존재하며,  기본적으론 master usrstack위로 쌓인다.  이때, thread create와 thread exit이 계속해서 호출된다면 계속 usrstack 위로 쌓여 master의 다른 segment의 영역과 충돌이 날 수 있으므로, 만약 workerthread가 이미 종료된 것이 있다면 그때 썼던 user스택의 위치를 다시 다져다가 쓴다.

> Thread Join

+ 인자로 받은 thread(pid)  값을 통해 기다려야 할 thread가 무엇인지 확인한 다음 만약 이미 끝났으면 0을 리턴하고 아니라면 끝날때까지 기다리다 끝난뒤에 thread exit에서 wakeup을 통해 돌아오면 해당 thread의 user stack을 dealloc하고 master가 관리하는 workerthread에서 현재 thread 정리하고 쓰레드가 소속된 proc또한 UNUSED로 바꾸고 자원을 다 내려놓는다.

> Thread exit

+ 쓰레드에서 할 일을 다 하고 불리는 함수로, 열려있는 file을 다 닫고, 부모를 깨워 마지막 줄 shed 후에 join이 발동되게한다.  그런뒤 반환 값을 master의 retvals[thread->index]에 저장하고, 자신의 상태를 좀비로 만든다.

> Exit

+  어떤 thread가 exit을 호출하면 그의 마스터 쓰레드를 데려와 모든 process을 중지시키는 작업을 한다. 이때 다른 모든 같은그룹 thread들을 정리하는 것이 중요하다. Exit 후에 wait에서 자식 process를 정리하듯, workerthread도 정리해준다.

> Fork

+ 기본적으로 thread 또한 proc 구조체를 쓰고 있으므로 thread에서 fork를 부르면 해당 thread를 일반 process처럼 생각하여 그대로fork한다.



#### "not Yet develop... just think how to do"

> Exec

+ thread에서 불리면 master thread를 불러와서 exec작업을 시키며, 해당 master thread에서 다른 process로 넘어가긴 전 worker thread까지 깔끔하게 없애주어야 한다.

> Sbrk

+ 단순히 생각하면 스택 세그먼트를 위로 늘려주는 것인데 현재 worker thread가 master thread usr stack위를 차지하고 있으므로 늘릴려고 하면 반드시 어디선가 충돌이 날 것이다... worker thread의 모든 user stack을 위로 일괄적으로 옮긴뒤 세그먼트를 넓혀야 하지 않을까 생각한다.

> Kill

+ thread에서 kill(pid)가 불리면 pid에 맞는 thread를 찾아 그의 master의 exit을 발동시켜 master와 나머지 workerthread 또한 일관적으로 삭제되게 한다.

> Pipe

+ 

> Sleep

+ 어떤 worker thread가 sleep에 빠져있는데 master나 다른 workerthread의 exit을 통해 그룹이 사라지려고 한다면, 자고있는 worker 쓰레드를 한번 확인하고 exit한다.