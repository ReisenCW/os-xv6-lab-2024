#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

int
main(int argc, char* argv[]){
  int p2c_fd[2]; // parent to child
  int c2p_fd[2]; // child to parent
  char buf[20] = {0};
  pipe(p2c_fd);
  pipe(c2p_fd);
  int pid = fork();

  if(pid == 0){
    // child process
    close(p2c_fd[1]); // close parent write
    read(p2c_fd[0], buf, sizeof(buf)); // read from parent to buf

    printf("%d: received %s\n", getpid(), buf); // print received message

    close(c2p_fd[0]); // close parent read
    write(c2p_fd[1], "pong", 4); // send "pong" to parent
    exit(0);
  }
  else{
    // parent process
    close(p2c_fd[0]); // close child read
    write(p2c_fd[1], "ping", 4); // send "ping" to child
    
    close(c2p_fd[1]); // close child write

    read(c2p_fd[0], buf, sizeof(buf)); // read from child
    printf("%d: received %s\n", getpid(), buf); // print received message
    exit(0);
  }

  exit(0);
}