# Project 3



### Milestone 1 

> Goal: Implement sync system call



+ make write system call  to write on buffer cache, not direct to disk
  + `begin_op` 와  `end_op`사이에서 transaction이 일어난다. 원래는 commit중인 다른 파일이 없으면 바로 log_write 후 end_op에서 남아있는 transaction이 없으면 실행된다. 이때, `outstanding ==0`와 `log.lh.n > log.size/2`를 동시에 만족 할 때만 commit()이 작동하게 하여, 일정량 이상의 log가 차있지 않을때는, 바로 disk로 flush하지 않고 대기한다.

+ int sync
  + 현재 log_write를 통해 in-memory 상에  `log`에는 기록이 되어 있으나 아직 disk에 log와 data block에 변경된것이 저장 되지 않았다. 이때 유저가 sync 시스템 콜을 부르거나 `log.lh.n > log.size/2`일때만 disk로 log랑 data block 모두 flush된다.
  + `sync()`가 불리면 commit()을 작동시켜 in memory 상의 로그 기록(modified data in buffer cache)를 전부 disk에 flush한다.
+ int get_log_num
  + `log.lh.n`을 반환한다.



### Milestone 2

> Goal: Expand the maximum size of a file

+ Double indirect and Triple Indirect

  + 본래 XV6에서는 single indirect 밖에 없다. 따라서 파일 사이즈의 한계가 작을 수 밖에 없다. 이를 해결 하기 위해 Double  혹은 Triple Indirect를 구현한다.

    + inode의 addrs에서 Double Indirect, Triple Indirect를 위한 공간이 마련되야 하므로, fs.h에서 dinode의 addrs[NDIRECT +1]을 addrs[NDIRECT + 3]으로 변경하고, file.h의 inode 또한 마찬가지로 늘려준다.

      ```c
      fs.h  
      struct dinode {
          short type;           // File type
          short major;          // Major device number (T_DEV only)
          short minor;          // Minor device number (T_DEV only)
          short nlink;          // Number of links to inode in file system
          uint size;            // Size of file (bytes)
          uint addrs[NDIRECT+3];   // Data block addresses, add 2 indirection
        };
      ....
      file.h
       struct inode {
           ...
           ...
          short major;
          short minor;
          short nlink;
          uint size;
          uint addrs[NDIRECT+3]; // add 2 indirection
        };
      
      ```

      

    + 또한 fs.h에서 double indirect와 triple indirect의 크기를 정한다. 이때, 크기는 NINDIRECT에서 (BSIZE / sizeof(uint)) 만큼 address가 할당되므로 블럭 또한 같이 128로 맞추었다.

      ```c
      fs.h
          
      #define NDIRECT 10 // add 2 indirection minus 2 NDIRECT -> keep size addr size 13 (following project specification)
      #define NINDIRECT (BSIZE / sizeof(uint))
      #define NDOUINDIRECT (NINDIRECT * NINDIRECT)
      #define NTRIINDIRECT (NINDIRECT * NINDIRECT * NINDIRECT)
      #define MAXFILE (NDIRECT + NINDIRECT + NDOUINDIRECT + TRIINDIRECT)
      
      ...
      
      ```

      

    + FILE 사이즈가 더 커질 수 있으므로, param.h에서 FSSIZE을 40000까지 늘려준다.

  + fs.c의 bmap 함수는 inode 포인터와 byte offset을 가져와 해당하는 주소를 반환한다. 이때, bmap은 single indirect 뿐만 아니라 , double, triple indirect도 존재하므로, block -> address의 double과 block ->block -> address의 triple indirect를 위한 매핑 코드가 추가적으로 필요하다. 

    ```c
    fs.c
    	//single -> double, same as single but check(bread) block 	
        //one more time and then bread to get addr, 
        //tri also same as double, just block check one more time
        if(bn < NDOUINDIRECT){
    402     int index1 = bn / NINDIRECT;// index of block
    403     int index2 = bn % NINDIRECT;// index of offset
    404     //if double indirect is empty
    405     if((addrs = ip->addrs[NDRIECT + 1]) ==0)
    406         ip->addrs[NDIRECT + 1] = addr = balloc(ip->dev);
    407     //get addr data which is block
    408     bp = bread(ip->dev, addr);
    409     a = (uint*)bp->data;
    410     //if start of block is empty then alloc
    411     if((addr = a[index1]) == 0){
    412         a[index1] = addr = balloc(ip->dev);
    413         log_write(bp);
    414     }
    415     brelse(bp);
    
    ```

    

  + fs.c의 itrunc 함수도 bmap과 같이 자원을 해제하는데 있어, double 과 triple은 추가된 블록만큼 따로 또 해제 해 주어야 한다. 이를 위한 추가적인 free 코드가 필요하다.

    > similar to bmap solution

### Milestone 1 구현

> sync()

```c
log.c
    
241 int
242 sys_sync(void)
243 {
244    commit();
245    if(log.lh.n == 0)
246        return 0;
247    else
248        return -1;
249 }

```

+ commit()을 통해 in memory 상에 저장되어있는 log값을 disk에 적어준다.  

  이때, 제대로 commit()되었으면 log.lh.n은 0이 되야하기 때문에 0이면 0을 반환해준다.



> get_log_num()

```c
log.c
    
250 int
251 sys_get_log_num(void)
252 {
253     return log.lh.n;
254 }
```

+ log.lh.n을 반환한다.



> end_op()

```c
log.c
    
145 void
146 end_op(void)
147 {
148   int do_commit = 0;
149 
150   acquire(&log.lock);
151   log.outstanding -= 1;
152   if(log.committing)
153     panic("log.committing");
154   //if half of log space is already allocated, then do_commit else not
155   if(log.outstanding == 0 && log.lh.n > log.size/2){
156     do_commit = 1;
157     log.committing = 1;
158   }

```

+ write 후 바로 disk에 접근하지 않고 inmemory의 log에 절반 이상 차있을때만, commit이 되게 하였다.





### Milestone 2 구현

> Double, Triple Indirect 구현

+ inode, dinode의 addrs의 끝에 2개는 Double, Triple Indirect를 위한 공간이 된다.
  이에 따라, double, triple 용으로 addrs[NDIRECT + 1] -> addrs[NDIRECT + 3]

```
fs.h  
struct dinode {
    short type;           // File type
    short major;          // Major device number (T_DEV only)
    short minor;          // Minor device number (T_DEV only)
    short nlink;          // Number of links to inode in file system
    uint size;            // Size of file (bytes)
    uint addrs[NDIRECT+3];   // Data block addresses, add 2 indirection
  };
....
file.h
 struct inode {
     ...
     ...
    short major;
    short minor;
    short nlink;
    uint size;
    uint addrs[NDIRECT+3]; // add 2 indirection
  };
```



+ 총 addrs의 크기는 13이 유지되어야하므로(과제 명세 상)

  NDIRECT 12 -> 10으로 2 만큼 줄었다. (대신 second, triple indirect가 생김)

+ double indirect, triple indirect을 설정하고, MAXFILE또한 늘려준다.

```c
 24	#define NDIRECT 10 // inode and dinode 
 25 #define NINDIRECT (BSIZE / sizeof(uint))
 26 #define NDOUINDIRECT (NINDIRECT * NINDIRECT)
 27 #define NTRIINDIRECT (NINDIRECT * NINDIRECT * NINDIRECT)
 28 #define MAXFILE (NDIRECT + NINDIRECT + NDOUINDIRECT + NTRIINDIRECT)

```



> bmap(struct inode *ip, uint bn)

+ 추가되는 double, triple indirection은 
  single indirection과 기본적으로 설계는 같다.

  ```c
  fs.c
      
  401   bn -= NINDIRECT;
  402 
  403   if(bn < NDOUINDIRECT){
  404     int index1 = bn / NINDIRECT;// index of block
  405     int index2 = bn % NINDIRECT;// index of offset
  406     //if double indirect is empty
  ...
  413     //if start of block is empty then alloc
  414     if((addr = a[index1]) == 0){
  415         a[index1] = addr = balloc(ip->dev);
  416         log_write(bp);
  417     }
  418     brelse(bp);
  419 
  420     //read addr which is real address location
  421     bp = bread(ip->dev, addr);
  422     a = (uint*)bp->data;
  423     if((addr = a[index2]) == 0){
  424         a[index2] = addr = balloc(ip->dev);
  425         log_write(bp);
  426     }
  427     brelse(bp);
  428     return addr;   
  
  ```

  + fs.c의 bmap의 모습이다. single때와 비슷하게 NDOUINDIRECT보다 작은 체크해서 TRI인지 아닌지 체크하고 맞다면 bread로 다음 데이터(블럭)를 가져오고,  다음 데이터(주소)를 가져온 뒤 반환한다.

+ Triple Indirection 또한 위와 비슷하다.





> itrunc(struct inode * ip)

+ bmap의 double,triple과 비슷한 원리로 구현된다. single의 설계를 기반으로 추가적인 내부 블럭 및 주소를 free한다. (자세한 사항은 주석에 적어두었습니다.)

### Milestone 3 구현

> pread(int fd, void* addr, int n, int off)

+ read와 비슷하게 작동한다. 단 filepread는 offset을 f->off가 아닌 주어진 offset을 참조하며 주어진 offset만을 사용하여 f->off에 어떠한 영향을 끼치지 않는다.



> pwrite(int fd, void* addr, int n, int off)

+ write와 비슷하게 작동한다. 단 filepread는 offset을 f->off가 아닌 주어진 offset을 참조하며 주어진 offset만을 사용하여 f->off에 어떠한 영향을 끼치지 않는다.