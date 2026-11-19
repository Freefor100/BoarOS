/* Included by core_dump.c; uses its real crash/wait and ELF reader helpers. */
struct filter_mapping { void *address; size_t length; unsigned mask; int header; };
static void filter_set(unsigned mask)
{
 int fd=open("/proc/self/coredump_filter",O_WRONLY);CHECK(fd>=0);
 char b[32];int n=snprintf(b,sizeof b,"0x%x\n",mask);CHECK(write(fd,b,n)==n);CHECK(!close(fd));
}
static void *filter_map_file(int fd,int flags,int write_page)
{
 void *p=mmap(NULL,3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
 CHECK(mmap(p,2*page,PROT_READ|PROT_WRITE,flags|MAP_FIXED,fd,0)==p);
 if(write_page){volatile char first=((volatile char *)p)[page];(void)first;((volatile char *)p)[page]=42;}
 return p;
}
static void filter_inspect(struct filter_mapping *maps,unsigned count,unsigned mask)
{
 int fd=open("core",O_RDONLY);CHECK(fd>=0);Elf64_Ehdr e;CHECK(read(fd,&e,sizeof e)==sizeof e);
 for(unsigned m=0;m<count;m++){
  int found=0;for(unsigned i=0;i<e.e_phnum;i++){
   Elf64_Phdr p;CHECK(pread(fd,&p,sizeof p,e.e_phoff+i*sizeof p)==sizeof p);
   if(p.p_type!=PT_LOAD||p.p_vaddr!=(uintptr_t)maps[m].address)continue;
   uint64_t expected=(mask & maps[m].mask)?maps[m].length:((mask&16)&&maps[m].header?page:0);
   if(p.p_filesz!=expected)printf("FILTER MAP %u mask=%x got=%llu expected=%llu\n",m,mask,(unsigned long long)p.p_filesz,(unsigned long long)expected);
   CHECK(p.p_filesz==expected);found=1;break;
  }CHECK(found);
 }
 int vdso=0;uintptr_t base=getauxval(AT_SYSINFO_EHDR);CHECK(base);
 for(unsigned i=0;i<e.e_phnum;i++){Elf64_Phdr p;CHECK(pread(fd,&p,sizeof p,e.e_phoff+i*sizeof p)==sizeof p);
  if(p.p_type==PT_LOAD && p.p_vaddr==base){CHECK(p.p_filesz==p.p_memsz && p.p_filesz);vdso=1;}}
 CHECK(vdso);CHECK(!close(fd));
}
static void filter_map_checks(void)
{
 /* Each two-page mapping has an unmapped/protected guard, so classification
  * is not obscured by legitimate adjacent VMA merging. */
 uintptr_t vdso=getauxval(AT_SYSINFO_EHDR);CHECK(vdso);CHECK(!madvise((void *)vdso,page,MADV_DONTDUMP));
 puts("CORE FILTER preparing mappings");
 int fd=open("filter-data",O_CREAT|O_RDWR,0600);CHECK(fd>=0);CHECK(!ftruncate(fd,2*page));
 int elf=open("filter-elf",O_CREAT|O_RDWR,0600);CHECK(elf>=0);CHECK(!ftruncate(elf,2*page));CHECK(pwrite(elf,ELFMAG,4,0)==4);
 struct filter_mapping maps[11]={0};
 maps[0]=(struct filter_mapping){filter_map_file(fd,MAP_PRIVATE,0),2*page,4,0};
 maps[1]=(struct filter_mapping){filter_map_file(fd,MAP_PRIVATE,1),2*page,1|4,0};
 CHECK(!madvise(maps[1].address,2*page,MADV_DONTNEED)); /* COW history is sticky. */
 maps[2]=(struct filter_mapping){filter_map_file(fd,MAP_SHARED,1),2*page,8,0};
 maps[3]=(struct filter_mapping){filter_map_file(elf,MAP_PRIVATE,0),2*page,4,1};
 int deleted=open("filter-deleted",O_CREAT|O_RDWR,0600);CHECK(deleted>=0);CHECK(!ftruncate(deleted,2*page));
 maps[4]=(struct filter_mapping){filter_map_file(deleted,MAP_SHARED,1),2*page,2,0};CHECK(!unlink("filter-deleted"));
 maps[5]=(struct filter_mapping){filter_map_file(-1,MAP_SHARED|MAP_ANONYMOUS,1),2*page,2,0};
 maps[6]=(struct filter_mapping){filter_map_file(-1,MAP_PRIVATE|MAP_ANONYMOUS,1),2*page,1,0};
 maps[7]=(struct filter_mapping){filter_map_file(-1,MAP_PRIVATE|MAP_ANONYMOUS,0),2*page,1,0};
 int executable=open("filter-executable",O_CREAT|O_RDWR,0700);CHECK(executable>=0);CHECK(!ftruncate(executable,3*page));
 maps[8]=(struct filter_mapping){filter_map_file(executable,MAP_PRIVATE,0),2*page,4,1};
 maps[9]=(struct filter_mapping){filter_map_file(executable,MAP_PRIVATE,0),2*page,4,0};
 CHECK(mmap(maps[9].address,2*page,PROT_READ,MAP_PRIVATE|MAP_FIXED,executable,page)==maps[9].address);
 int shmid=shmget(IPC_PRIVATE,2*page,IPC_CREAT|0600);CHECK(shmid>=0);
 void *shm=shmat(shmid,NULL,0);CHECK(shm!=(void *)-1);((volatile char *)shm)[0]=3;
 maps[10]=(struct filter_mapping){shm,2*page,2,0};CHECK(!shmctl(shmid,IPC_RMID,NULL));
 /* Ordinary anonymous first-read then write must not need a new policy
  * fault or silently disappear from the anonymous category. */
 volatile char first=((volatile char *)maps[7].address)[0];CHECK(first==0);
 ((volatile char *)maps[7].address)[0]=17;
 unsigned masks[]={0,1,2,4,8,16,0x33,0x1ff};
 for(unsigned i=0;i<sizeof masks/sizeof masks[0];i++){
  printf("CORE FILTER mask=%x\n",masks[i]);filter_set(masks[i]);CHECK(WCOREDUMP(crash(8)));filter_inspect(maps,11,masks[i]);CHECK(!unlink("core"));
 }
 /* DONTDUMP takes precedence over all enabled categories. */
 CHECK(!madvise(maps[1].address,2*page,MADV_DONTDUMP));maps[1].mask=0;
 filter_set(0x1ff);CHECK(WCOREDUMP(crash(8)));filter_inspect(maps,11,0x1ff);CHECK(!unlink("core"));
 CHECK(!shmdt(shm));CHECK(!close(executable)&&!unlink("filter-executable"));
 for(unsigned i=0;i<10;i++)CHECK(!munmap(maps[i].address,3*page));
 CHECK(!close(fd)&&!close(elf)&&!close(deleted));CHECK(!unlink("filter-data")&&!unlink("filter-elf"));
 filter_set(0x33);puts("CORE CASE filter-categories-headers-cow-discard-vdso PASS");
}
static void filter_huge_checks(void)
{
 FILE *f=fopen("/proc/meminfo","r");CHECK(f);char line[128];size_t huge=0;
 while(fgets(line,sizeof line,f))if(!strncmp(line,"Hugepagesize:",13)){unsigned long kb;CHECK(sscanf(line+13,"%lu",&kb)==1);huge=kb*1024;}
 CHECK(!fclose(f)&&huge>page);
 int fd=open("/proc/sys/vm/nr_hugepages",O_WRONLY);CHECK(fd>=0);CHECK(write(fd,"3",1)==1);CHECK(!close(fd));
 struct filter_mapping maps[2];
 for(unsigned i=0;i<2;i++){
  void *p=mmap(NULL,huge,PROT_READ|PROT_WRITE,MAP_ANONYMOUS|MAP_HUGETLB|(i?MAP_SHARED:MAP_PRIVATE),-1,0);CHECK(p!=MAP_FAILED);
  ((volatile char *)p)[0]=31;maps[i]=(struct filter_mapping){p,huge,i?64:32,0};
 }
 unsigned masks[]={0x33,64};
 for(unsigned i=0;i<2;i++){
  printf("CORE FILTER mask=%x\n",masks[i]);filter_set(masks[i]);CHECK(WCOREDUMP(crash(8)));filter_inspect(maps,2,masks[i]);CHECK(!unlink("core"));
 }
 for(unsigned i=0;i<2;i++)CHECK(!munmap(maps[i].address,huge));
 fd=open("/proc/sys/vm/nr_hugepages",O_WRONLY);CHECK(fd>=0);CHECK(write(fd,"0",1)==1);CHECK(!close(fd));
 filter_set(0x33);puts("CORE CASE filter-private-shared-hugetlb PASS");
}
static void filter_cold_limit_checks(void)
{
 const size_t cold=64*1024*1024,limit=8*1024*1024;
 char *range=mmap(NULL,cold+3*page,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(range!=MAP_FAILED);
 CHECK(mmap(range,cold,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==range);
 char *hot=range+cold+page;
 CHECK(mmap(hot,page,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==hot);
 memcpy(hot,"after cold map",15);filter_set(0x33);
 CHECK(WCOREDUMP(crash(9)));int fd=open("core",O_RDONLY);CHECK(fd>=0);
 struct stat st;CHECK(!fstat(fd,&st)&&st.st_size<=(off_t)limit);
 Elf64_Ehdr e;CHECK(read(fd,&e,sizeof e)==sizeof e);unsigned seen=0;
 for(unsigned i=0;i<e.e_phnum;i++){
  Elf64_Phdr p;CHECK(pread(fd,&p,sizeof p,e.e_phoff+i*sizeof p)==sizeof p);if(p.p_type!=PT_LOAD)continue;
  if(p.p_vaddr==(uintptr_t)range){CHECK(p.p_memsz==cold && !p.p_filesz);seen|=1;}
  if(p.p_vaddr==(uintptr_t)hot){char bytes[15];CHECK(p.p_filesz==page);
   CHECK(pread(fd,bytes,sizeof bytes,p.p_offset)==sizeof bytes);CHECK(!memcmp(bytes,"after cold map",15));seen|=2;}
 }
 CHECK(seen==3);CHECK(!close(fd)&&!unlink("core"));CHECK(!munmap(range,cold+3*page));
 puts("CORE CASE cold-anonymous-low-limit-later-payload PASS");
}
