#include "kernel/types.h"
#include "kernel/stat.h"
#include "user/user.h"
#include "kernel/fs.h"
#include "kernel/fcntl.h"

char*
fmtname(char *path)
{
  static char buf[DIRSIZ+1];
  char *p;

  // Find first character after last slash.
  for(p=path+strlen(path); p >= path && *p != '/'; p--);
  p++;

  // Return blank-padded name.
  if(strlen(p) >= DIRSIZ)
    return p;
  memmove(buf, p, strlen(p));
  memset(buf+strlen(p), ' ', DIRSIZ-strlen(p));
  return buf;
}

void find(char* path, char* file_name){
  char buf[512], *p;
  int fd;
  struct dirent de;
  struct stat st;

  // 打开当前路径
  if((fd = open(path, O_RDONLY)) < 0){
    fprintf(2, "find: cannot open %s\n", path);
    return;
  }
  if(fstat(fd, &st) < 0){
    fprintf(2, "find: cannot stat %s\n", path);
    close(fd);
    return;
  }
  switch(st.type){
    case T_DEVICE:
    case T_FILE:
      // 若当前路径为文件或设备,只检查是否与目标文件名相同
      if(strcmp(fmtname(path), file_name) == 0){
        printf("%s\n", path);
      }
      break;
    case T_DIR:
      // 若当前路径为文件夹,开始进入查找
      // 路径太长
      if(strlen(path) + 1 + DIRSIZ + 1 > sizeof buf){
        fprintf(2, "ls: path too long\n");
        break;
      }
      strcpy(buf, path);
      p = buf + strlen(buf);
      *p++ = '/';
      while(read(fd, &de, sizeof(de)) == sizeof(de)){
        if(de.inum == 0) continue;
        if(!strcmp(de.name, ".") || !strcmp(de.name, "..")) continue; // 跳过.和..

        memmove(p, de.name, DIRSIZ); // 拼接路径
        p[DIRSIZ] = 0;

        if(stat(buf, &st) < 0){
          fprintf(2, "find: cannot stat %s\n", buf);
          continue;
        }
        // 如果当前路径是目录
        if(st.type == T_DIR){
          // 递归查找
          find(buf, file_name);
        } 
        // 文件名匹配则打印
        else if(st.type == T_FILE && strcmp(de.name, file_name) == 0){
          printf("%s\n", buf);
        }
      }
  }
  close(fd);
}

int main(int argc, char* argv[]){
  if(argc != 3){
    fprintf(2, "Usage: find <path> <file_name>\n");
    exit(1);
  }
  find(argv[1], argv[2]);

  exit(0);
}

