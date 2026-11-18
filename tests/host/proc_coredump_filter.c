/* Real proc routing/parser; MM and identity providers modeled. */
#define PROC_FILTER_PROVIDER
#define PROC_HUGE_NO_MAIN
#include "proc_hugetlb.c"
static unsigned mask=0x33;static int live=1;
int kernel_proc_coredump_filter(kernel_pid_t p,uint64_t i,int t,unsigned *v,int w)
{assert(p==42&&i==7&&!t);if(!live)return w?-KERNEL_ESRCH:0;if(w)mask=*v&0x1ff;else *v=mask;return 1;}
int main(void)
{
 uint64_t inode,offset=0;uint32_t mode;size_t count;
 assert(!proc_lookup(0,proc_pid_inode(42,7,PROC_PID_DIR_KIND),"coredump_filter",15,&inode,&mode));
 assert(mode==(KERNEL_VFS_S_IFREG|0644)&&proc_inode_kind(inode)==PROC_PID_FILTER_KIND&&!proc_inode_is_thread(inode));
 uint64_t ignored;assert(proc_lookup(0,proc_pid_inode(43,8,PROC_TID_DIR_KIND),"coredump_filter",15,&ignored,&mode)==-KERNEL_ENOENT);
 struct kernel_vfs_node node={.inode=inode};char text[64]={0};
 assert(!proc_control(&node,0,&offset,text,4,&count)&&count==4&&!memcmp(text,"0000",4));
 assert(!proc_control(&node,0,&offset,text,sizeof(text),&count)&&count==5&&!memcmp(text,"0033\n",5));
 assert(!proc_control(&node,0,&offset,text,sizeof(text),&count)&&!count);
 const char *good[]={"51","063","0x33","+0X33\n","4294967295"};
 for(unsigned i=0;i<5;i++){strcpy(text,good[i]);assert(!proc_control(&node,1,&offset,text,strlen(text),&count));assert(mask==(i==4?511:51)&&offset==9);}
 const char *bad[]={"","-1","08","0x"," 1","1 ","1\n\n","4294967296","18446744073709551616","4294967296x"};
 for(unsigned i=0;i<10;i++){strcpy(text,bad[i]);assert(proc_control(&node,1,&offset,text,strlen(text),&count)==((i==7||i==8)?-KERNEL_ERANGE:-KERNEL_EINVAL));assert(mask==511);}
 memset(text,0,sizeof(text));text[0]='7';assert(!proc_control(&node,1,&offset,text,34,&count)&&count==34&&mask==7);
 live=0;offset=0;assert(!proc_control(&node,0,&offset,text,9,&count)&&!count);assert(proc_control(&node,1,&offset,text,1,&count)==-KERNEL_ESRCH);
 puts("proc coredump_filter: routing, mode, base/overflow, partial reads, unchanged offset, no-MM EOF/ESRCH PASS");
}
