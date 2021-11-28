#include "types.h"
#include "stat.h"
#include "user.h"

int
main(int argc, char *argv[]){
   int check = fork();
   while(1){
       
       if(check > 0){
           write(1, "parent\n", 7);
           
           yield();
       }
        else if(check == 0){
           write(1, "child\n",6);
           
           yield();
       }
        else{
          printf(1,"error\n");
          exit();
       }
   }
   wait();
   exit();
}
