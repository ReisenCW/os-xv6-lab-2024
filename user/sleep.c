#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int main(int argc, char* argv[]){
  // 参数过少
  if(argc != 2){
    fprintf(2,"parameter number error");
    exit(1);
  }
  else{
    int t = atoi(argv[1]);
    sleep(t);
  }
  exit(0);
}
