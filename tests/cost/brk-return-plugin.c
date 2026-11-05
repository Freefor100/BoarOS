/* 只读记录已校验原ELF包装器的syscall返回与函数返回，不改寄存器。 */
#include <qemu-plugin.h>
#include <stdio.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version=QEMU_PLUGIN_VERSION;
static struct qemu_plugin_register *a0,*a7;
static GByteArray *bytes;
static FILE *trace;
static uint64_t pending_pc;
static int loongarch;
struct point { uint64_t pc; unsigned opcode; };
static uint64_t reg(struct qemu_plugin_register *r)
{
    g_byte_array_set_size(bytes,0);
    if(!qemu_plugin_read_register(r,bytes)||bytes->len!=8)g_error("register unreadable");
    uint64_t value;memcpy(&value,bytes->data,8);return GUINT64_FROM_LE(value);
}
static void execute(unsigned cpu,void *data)
{
    (void)cpu;struct point *p=data;
    unsigned mask=loongarch ? 0x3fff : 0xfff;
    unsigned syscall_offset=loongarch ? 0x2474 : 0xe0c;
    if((p->pc&mask)==syscall_offset && reg(a7)==214){
        pending_pc=p->pc;
        fprintf(trace,"BRK CALL requested=%lx\n",reg(a0));
    }else if(pending_pc && p->pc==pending_pc+4){
        unsigned trunc=loongarch ? 0x00408084 : 0x2501;
        unsigned nop=loongarch ? 0x03400000 : 0x0001;
        if(p->opcode!=trunc && p->opcode!=nop)g_error("unexpected brk return instruction");
        fprintf(trace,"BRK RAW mode=%s value=%lx\n",p->opcode==trunc ? "original" : "adapted",reg(a0));
    }else if(pending_pc && p->pc==pending_pc+(loongarch ? 8 : 6)){
        fprintf(trace,"BRK USER value=%lx\n",reg(a0));pending_pc=0;
    }
}
static void translate(struct qemu_plugin_tb *tb,void *unused)
{
    (void)unused;
    unsigned mask=loongarch ? 0x3fff : 0xfff,offset=loongarch ? 0x2474 : 0xe0c;
    for(size_t i=0;i<qemu_plugin_tb_n_insns(tb);i++){
        struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);
        uint64_t pc=qemu_plugin_insn_vaddr(in);unsigned low=pc&mask;
        if(low!=offset && low!=offset+4 && low!=offset+(loongarch ? 8 : 6))continue;
        struct point *p=g_new0(struct point,1);p->pc=pc;
        qemu_plugin_insn_data(in,&p->opcode,sizeof(p->opcode));p->opcode=GUINT32_FROM_LE(p->opcode);
        if(qemu_plugin_insn_size(in)==2)p->opcode&=0xffff;
        if(low==offset && p->opcode!=(loongarch ? 0x002b0000 : 0x73))continue;
        qemu_plugin_register_vcpu_insn_exec_cb(in,execute,QEMU_PLUGIN_CB_R_REGS,p);
    }
}
static void init(unsigned cpu,void *unused)
{
    (void)unused;if(cpu)g_error("one CPU required");
    GArray *all=qemu_plugin_get_registers();
    for(unsigned i=0;i<all->len;i++){
        qemu_plugin_reg_descriptor *r=&g_array_index(all,qemu_plugin_reg_descriptor,i);
        if(!strcmp(r->name,loongarch ? "r4" : "a0"))a0=r->handle;
        if(!strcmp(r->name,loongarch ? "r11" : "a7"))a7=r->handle;
    }
    g_array_free(all,TRUE);if(!a0||!a7)g_error("missing argument registers");bytes=g_byte_array_new();
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv)
{
    if(argc!=1 || strncmp(argv[0],"trace=",6) || !(trace=fopen(argv[0]+6,"w")))return -1;
    loongarch=!strcmp(info->target_name,"loongarch64");
    if(!info->system_emulation || (!loongarch && strcmp(info->target_name,"riscv64")))return -1;
    setvbuf(trace,NULL,_IOLBF,0);
    qemu_plugin_register_vcpu_init_cb(id,init,NULL);qemu_plugin_register_vcpu_tb_trans_cb(id,translate,NULL);
    return 0;
}
