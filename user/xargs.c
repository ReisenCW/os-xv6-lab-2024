#include "kernel/types.h"
#include "kernel/stat.h"
#include "kernel/fcntl.h"
#include "user/user.h"
#include "kernel/param.h"

void RemoveNewLine(char* str) {
  int len = strlen(str);
  if(len > 0 && str[len - 1] == '\n') {
    str[len - 1] = '\0';
  }
}

int main(int argc, char *argv[]) {
  char* cmd_args[MAXARG];
  int cmd_argc = 0;
  if(argc < 2) {
    fprintf(2, "usage: xargs command [args...]\n");
    exit(1);
  }
  
  // 保存原始命令参数（如 "grep", "hello"）
  for(int i = 1; i < argc; i++) {
    cmd_args[cmd_argc++] = argv[i];
  }
  
  // 从stdin读取参数并执行命令
  char line[128];
  while(gets(line, sizeof(line))) {
    RemoveNewLine(line);
    if(strlen(line) == 0) break; // EOF时退出
    
    // 创建完整参数列表：原始参数 + 新参数
    char* full_args[MAXARG];
    int full_argc = 0;
    
    // 复制原始参数
    for(int i = 0; i < cmd_argc; i++) {
      full_args[full_argc++] = cmd_args[i];
    }
    
    // 添加新参数（每行一个）
    full_args[full_argc++] = line;
    full_args[full_argc] = 0; // NULL结尾
    
    // 执行命令
    if(fork() == 0) {
      exec(full_args[0], full_args);
      exit(1); // exec失败才会执行到这里
    } else {
      wait(0); // 等待子进程完成
    }
  }

  exit(0);
}