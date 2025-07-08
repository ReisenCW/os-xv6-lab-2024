#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"

void generate_nums(int* nums, int len);
void recur_primes(int read_fd) __attribute__((noreturn));
void send_numbers(int* nums, int len, int write_fd); 

int main(int, char**){
  int fd[2];
  pipe(fd);
  
  if(fork() == 0){
    // child process
    close(fd[1]);  // Close unused write end
    recur_primes(fd[0]);
    close(fd[0]);  // Close read end after use
    exit(0);
  }
  else{
    // parent process
    close(fd[0]);  // Close unused read end
    int nums[278];  // Numbers 2-100
    generate_nums(nums, 278);
    send_numbers(nums, 278, fd[1]);
    close(fd[1]);  // Close write end
    wait(0);       // Wait for child to finish
    exit(0);
  }
}

void generate_nums(int* nums, int len){
  for(int i = 0; i < len; i++){
    nums[i] = i + 2;
  }
}

void recur_primes(int read_fd) {
  int p;
  
  // Read first prime
  if (read(read_fd, &p, sizeof(p)) <= 0) {
    close(read_fd);
    exit(0);
  }
  printf("prime %d\n", p);
  
  int new_pipe[2];
  pipe(new_pipe);
  
  if (fork() == 0) {
    // Child process for next level
    close(new_pipe[1]);  // Close unused write end
    close(read_fd);      // Close parent's pipe (not needed anymore)
    recur_primes(new_pipe[0]);
    close(new_pipe[0]);  // Close read end after use
    exit(0);
  } 
  else {
    // Parent process - filter numbers
    close(new_pipe[0]);  // Close unused read end
    int num;
    
    while (read(read_fd, &num, sizeof(num)) > 0) {
      if (num % p != 0) {
        write(new_pipe[1], &num, sizeof(num));
      }
    }
    
    // Close all file descriptors
    close(read_fd);
    close(new_pipe[1]);
    
    // Wait for child to finish
    wait(0);
    exit(0);
  }
}

void send_numbers(int* nums, int len, int write_fd) {
  for(int i = 0; i < len; i++){
    write(write_fd, &nums[i], sizeof(nums[i]));
  }
}