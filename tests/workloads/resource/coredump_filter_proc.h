/* Linux v6.15 proc/base.c + lib/kstrtox.c observable ABI checks. */
static unsigned filter_read(void)
{
 int fd=open("/proc/self/coredump_filter",O_RDONLY);CHECK(fd>=0);
 char text[16]={0};CHECK(read(fd,text,sizeof(text))==9);CHECK(read(fd,text,sizeof(text))==0);CHECK(!close(fd));
 unsigned value;CHECK(sscanf(text,"%x",&value)==1);return value;
}
static void filter_write(const char *text)
{
 int fd=open("/proc/self/coredump_filter",O_WRONLY);CHECK(fd>=0);
 CHECK(write(fd,text,strlen(text))==(ssize_t)strlen(text));CHECK(lseek(fd,0,SEEK_CUR)==0);CHECK(!close(fd));
}
static void *filter_thread(void *unused)
{(void)unused;filter_write("0x17");return NULL;}
static int filter_proc_exec_check(int argc,char **argv)
{
 if(argc!=2 || strcmp(argv[1],"--filter-exec"))return 0;
 CHECK(filter_read()==0x19);return 1;
}
static void filter_proc_checks(void)
{
 unsigned saved=filter_read();struct stat st;CHECK(!stat("/proc/self/coredump_filter",&st));CHECK((st.st_mode&0777)==0644);
 int fd=open("/proc/self/coredump_filter",O_RDWR);CHECK(fd>=0);
 const char *good[]={"51","063","0X33","+0x33\n","0xffffffff"};
 for(unsigned i=0;i<sizeof(good)/sizeof(*good);i++){CHECK(write(fd,good[i],strlen(good[i]))==(ssize_t)strlen(good[i]));CHECK(filter_read()==(i==4?0x1ff:0x33));}
 const char *bad[]={""," ","-1","08","0x","1 ","1\n\n","4294967296","18446744073709551616"};
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){errno=0;CHECK(write(fd,bad[i],strlen(bad[i]))==-1);CHECK(errno==(i>=7?ERANGE:EINVAL));}
 char embedded[]={'5','1',0,'x'};CHECK(write(fd,embedded,sizeof(embedded))==sizeof(embedded));CHECK(filter_read()==0x33);
 CHECK(pwrite(fd,"7",1,123)==1);CHECK(filter_read()==7);CHECK(lseek(fd,0,SEEK_CUR)==0);
 char text[10]={0};CHECK(pread(fd,text,4,5)==4);CHECK(!memcmp(text,"007\n",4));
 struct iovec vec[2]={{"3",1},{"5",1}};CHECK(writev(fd,vec,2)==2);CHECK(filter_read()==5);
 char *guard=mmap(NULL,2*page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(guard!=MAP_FAILED);CHECK(!mprotect(guard+page,page,PROT_NONE));
 char *tail=guard+page-34;memset(tail,0,34);tail[0]='9';CHECK(write(fd,tail,64)==64);CHECK(filter_read()==9);
 errno=0;CHECK(write(fd,guard+page-1,2)==-1 && errno==EFAULT);CHECK(filter_read()==9);
 vec[0]=(struct iovec){"11",2};vec[1]=(struct iovec){guard+page,1};CHECK(writev(fd,vec,2)==2);CHECK(filter_read()==11);
 CHECK(!munmap(guard,2*page));CHECK(!close(fd));
 char path[128];snprintf(path,sizeof(path),"/proc/self/task/%ld/coredump_filter",syscall(SYS_gettid));errno=0;CHECK(open(path,O_RDONLY)==-1&&errno==ENOENT);
 errno=0;CHECK(open("/proc/thread-self/coredump_filter",O_RDONLY)==-1&&errno==ENOENT);
 pthread_t thread;CHECK(!pthread_create(&thread,NULL,filter_thread,NULL));CHECK(!pthread_join(thread,NULL));CHECK(filter_read()==0x17);
 pid_t pid=fork();CHECK(pid>=0);if(!pid){CHECK(filter_read()==0x17);filter_write("0x19");execl("/proc/self/exe","core_dump","--filter-exec",NULL);_exit(99);}
 int status;CHECK(waitpid(pid,&status,0)==pid && WIFEXITED(status)&&WEXITSTATUS(status)==0);CHECK(filter_read()==0x17);
 snprintf(text,sizeof(text),"0x%x",saved);filter_write(text);
 puts("CORE coredump_filter proc PASS");
}
