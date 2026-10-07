/* 固定原ELF的只读观测；显式故障profile仅在Linux返回边界注入-EAGAIN。 */
#include <qemu-plugin.h>
#include <stdio.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
static struct qemu_plugin_register *registers[7];
static GByteArray *bytes,*memory;
static FILE *trace;
static unsigned pending;
static int inject_error;
struct instruction { uint64_t pc;uint32_t opcode;char *assembly; };
static uint64_t read_register(unsigned index)
{
    g_byte_array_set_size(bytes,0);
    if(!qemu_plugin_read_register(registers[index],bytes)||bytes->len!=8)
        g_error("RV register unavailable");
    uint64_t value;memcpy(&value,bytes->data,8);return GUINT64_FROM_LE(value);
}
static int read_errno(uint64_t tp)
{
    if(!qemu_plugin_read_memory_vaddr(tp+ERRNO_OFFSET,memory,4))g_error("TLS errno unreadable");
    int value;memcpy(&value,memory->data,4);return GINT32_FROM_LE(value);
}
static void execute(unsigned cpu,void *data)
{
    (void)cpu;struct instruction *in=data;
    uint64_t tp=read_register(4);
    if(read_register(5)!=0 || !tp)return;
    if(pending){
        if(inject_error && pending==278){
            uint64_t value=GUINT64_TO_LE((uint64_t)-11);
            fprintf(trace,"INJECT actual=%ld visible=-11\n",(int64_t)read_register(0));
            g_byte_array_set_size(bytes,0);g_byte_array_append(bytes,(guint8*)&value,8);
            if(!qemu_plugin_write_register(registers[0],bytes))g_error("error injection failed");
        }
        fprintf(trace,"RETURN nr=%u pc=%lx result=%ld errno=%d\n",pending,in->pc,(int64_t)read_register(0),read_errno(tp));
        pending=0;
    }
    if(in->pc==MALLOC_INIT_PC)fprintf(trace,"MALLOC-INIT pc=%lx caller=%lx errno=%d\n",in->pc,read_register(6),read_errno(tp));
    if(in->pc==MAIN_PC)fprintf(trace,"MAIN pc=%lx errno=%d\n",in->pc,read_errno(tp));
    if(in->pc==CLOCK_RETURN_PC)fprintf(trace,"CLOCK-RESULT pc=%lx result=%ld errno=%d\n",in->pc,(int64_t)read_register(0),read_errno(tp));
    if(in->opcode==0x73 && (read_register(3)==113 || read_register(3)==278)){
        pending=(unsigned)read_register(3);
        fprintf(trace,"CALL nr=%u pc=%lx a0=%lx a1=%lx a2=%lx errno=%d\n",pending,in->pc,read_register(0),read_register(1),read_register(2),read_errno(tp));
    }
}
static void store(unsigned cpu,qemu_plugin_meminfo_t info,uint64_t address,void *data)
{
    (void)cpu;struct instruction *in=data;uint64_t tp=read_register(4);
    if(read_register(5)!=0 || !tp || address!=tp+ERRNO_OFFSET || qemu_plugin_mem_size_shift(info)!=2)return;
    qemu_plugin_mem_value value=qemu_plugin_mem_get_value(info);
    fprintf(trace,"ERRNO-STORE pc=%lx offset=%u value=%u asm=%s\n",in->pc,ERRNO_OFFSET,value.data.u32,in->assembly);
}
static void translate(struct qemu_plugin_tb *tb,void *unused)
{
    (void)unused;
    for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){
        struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);uint64_t pc=qemu_plugin_insn_vaddr(in);
        if(pc<TEXT_START || pc>=TEXT_END)continue;
        struct instruction *record=g_new0(struct instruction,1);
        record->pc=pc;record->assembly=qemu_plugin_insn_disas(in);
        qemu_plugin_insn_data(in,&record->opcode,sizeof(record->opcode));record->opcode=GUINT32_FROM_LE(record->opcode);
        qemu_plugin_register_vcpu_insn_exec_cb(in,execute,inject_error?QEMU_PLUGIN_CB_RW_REGS:QEMU_PLUGIN_CB_R_REGS,record);
        qemu_plugin_register_vcpu_mem_cb(in,store,QEMU_PLUGIN_CB_R_REGS,QEMU_PLUGIN_MEM_W,record);
    }
}
static void init(unsigned cpu,void *unused)
{
    (void)unused;if(cpu)g_error("diagnostic requires one CPU");
    const char *names[]={"a0","a1","a2","a7","tp","priv","ra"};GArray *all=qemu_plugin_get_registers();
    for(unsigned i=0;i<all->len;i++){
        qemu_plugin_reg_descriptor *r=&g_array_index(all,qemu_plugin_reg_descriptor,i);
        for(unsigned j=0;j<7;j++)if(!strcmp(r->name,names[j]))registers[j]=r->handle;
    }
    for(unsigned j=0;j<7;j++)if(!registers[j])g_error("missing %s register",names[j]);
    g_array_free(all,TRUE);bytes=g_byte_array_new();memory=g_byte_array_new();
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv)
{
    if((argc!=1 && argc!=2)||strncmp(argv[0],"trace=",6)||!(trace=fopen(argv[0]+6,"w")))return -1;
    if(argc==2){if(strcmp(argv[1],"fault-getrandom=on"))return -1;inject_error=1;}
    if(strcmp(info->target_name,"riscv64")||!info->system_emulation)return -1;
    setvbuf(trace,NULL,_IOLBF,0);
    qemu_plugin_register_vcpu_init_cb(id,init,NULL);qemu_plugin_register_vcpu_tb_trans_cb(id,translate,NULL);return 0;
}
