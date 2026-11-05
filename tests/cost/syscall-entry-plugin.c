/* RV64 system-emulation observer; original guest binaries remain unchanged. */
#include <qemu-plugin.h>
#include <stdio.h>
#include <string.h>
QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;
static struct qemu_plugin_register *regs[9];
static GByteArray *bytes;
static unsigned count;
static FILE *trace;
static uint64_t read_reg(unsigned i)
{
    g_byte_array_set_size(bytes, 0);
    if (!qemu_plugin_read_register(regs[i], bytes) || bytes->len != 8)
        g_error("RV64 register unavailable");
    uint64_t value; memcpy(&value, bytes->data, 8); return GUINT64_FROM_LE(value);
}
static void entry(unsigned cpu, void *pc)
{
    (void)cpu;
    if (read_reg(8) != 0 || count++ >= 20000) return;
    fprintf(trace, "ECALL pc=%lx nr=%lu a0=%lx a1=%lx a2=%lx\n",
            (unsigned long)pc, read_reg(7), read_reg(0), read_reg(1), read_reg(2));
}
static void translate(struct qemu_plugin_tb *tb, void *unused)
{
    (void)unused;
    for (size_t i=0; i<qemu_plugin_tb_n_insns(tb); ++i) {
        struct qemu_plugin_insn *in=qemu_plugin_tb_get_insn(tb,i);
        uint32_t opcode=0; qemu_plugin_insn_data(in,&opcode,sizeof(opcode));
        if (GUINT32_FROM_LE(opcode)==0x73)
            qemu_plugin_register_vcpu_insn_exec_cb(in,entry,QEMU_PLUGIN_CB_R_REGS,
                (void *)(uintptr_t)qemu_plugin_insn_vaddr(in));
    }
}
static void init(unsigned cpu, void *unused)
{
    (void)unused; if(cpu) g_error("diagnostic requires one hart");
    GArray *all=qemu_plugin_get_registers();
    const char *names[]={"a0","a1","a2","a3","a4","a5","a6","a7","priv"};
    for(unsigned i=0;i<all->len;i++) {
        qemu_plugin_reg_descriptor *r=&g_array_index(all,qemu_plugin_reg_descriptor,i);
        for(unsigned j=0;j<9;j++) if(!strcmp(r->name,names[j])) regs[j]=r->handle;
    }
    for(unsigned j=0;j<9;j++) if(!regs[j]) g_error("missing %s register",names[j]);
    g_array_free(all,TRUE); bytes=g_byte_array_new();
}
QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id,const qemu_info_t *info,int argc,char **argv)
{
    if (argc != 1 || strncmp(argv[0],"trace=",6) || !(trace=fopen(argv[0]+6,"w"))) return -1;
    setvbuf(trace,NULL,_IOLBF,0);
    if(strcmp(info->target_name,"riscv64") || !info->system_emulation) return -1;
    qemu_plugin_register_vcpu_init_cb(id,init,NULL);
    qemu_plugin_register_vcpu_tb_trans_cb(id,translate,NULL); return 0;
}
