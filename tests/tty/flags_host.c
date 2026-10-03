/* Independent observable line-discipline tests against actual fs/tty.c.
 * Fixed oracle: references/linux/drivers/tty/n_tty.c, f4cdf7ca9a1f.
 * Dependencies and transport boundary are shared with the basic host fixture. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wreturn-type"
#define main basic_main
#include "core_host.c"
#undef main
#pragma GCC diagnostic pop
#define F_IGNBRK 1U
#define F_BRKINT 2U
#define F_IGNPAR 4U
#define F_PARMRK 8U
#define F_INPCK 16U
#define F_ISTRIP 32U
#define F_INLCR 64U
#define F_IGNCR 128U
#define F_ICRNL 256U
#define F_IXON 1024U
#define F_IXANY 2048U
#define F_IXOFF 4096U
#define F_IUTF8 16384U
#define L_ISIG 1U
#define L_ICANON 2U
#define L_ECHO 8U
#define L_ECHOE 16U
#define L_ECHOK 32U
#define L_NOFLSH 128U
#define L_ECHOCTL 512U
#define L_ECHOKE 2048U
#define L_IEXTEN 32768U
static unsigned char result[8192];
static void start_case(void) {
    memset(&task,0,sizeof(task));memset(&other,0,sizeof(other));
    output_size=0;credit=sizeof(output);now_ns=0;fault_address=0;
    memset(signals,0,sizeof(signals));
    assert(!live&&!tty&&identity.references==1);
    assert(!kernel_tty_create(&heap,&transport,0,&tty));
    kernel_tty_publish_serial(tty);device=kernel_tty_device_lookup(0x440);
    assert(!device->open(&heap,&task,0400,&instance));
}
static void end_case(void) {
    kernel_tty_shutdown(tty);device->release(instance);instance=0;
    assert(!kernel_tty_destroy(&tty)&&!live&&identity.references==1);
}
static void mode(unsigned iflag,unsigned oflag,unsigned lflag) {
    struct kernel_tty_termios s;termios(&s);s.iflag=iflag;s.oflag=oflag;s.lflag=lflag;
    s.cc[6]=0;s.cc[5]=0;settings(&s);flush();output_size=0;credit=sizeof(output);
}
static void check_read(const unsigned char *expected,size_t n) {
    int64_t got=read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK);
    if(got!=(int64_t)n||memcmp(result,expected,n)) {
        fprintf(stderr,"read got=%lld expected=%zu bytes:",(long long)got,n);
        if(got>0)for(int64_t i=0;i<got;i++)fprintf(stderr," %02x",result[i]);
        fputc('\n',stderr);abort();
    }
}
static void check_output(const unsigned char *expected,size_t n) {
    credit=sizeof(output);while(kernel_tty_service_output(tty,256)){}
    if(output_size!=n||memcmp(output,expected,n)) {
        fprintf(stderr,"output got=%zu expected=%zu bytes:",output_size,n);
        for(size_t i=0;i<output_size&&i<100;i++)fprintf(stderr," %02x",output[i]);
        fputc('\n',stderr);abort();
    }
}
static void write_text(const unsigned char *p,size_t n) {
    size_t written=0;assert(!device->write(instance,&task,KERNEL_FILES_O_NONBLOCK,p,n,&written)&&written==n);
}
static void flagged(unsigned char c,unsigned char status) {
    struct kernel_tty_rx r={c,status};kernel_tty_receive(tty,&r,1);
}
static void controlling(void) {
    task.leader=1;assert(!device->ioctl(instance,&task,0,0,0,0x540e,0));
}
static void transforms(void) {
    mode(F_ISTRIP|F_ICRNL|F_INLCR,0,0);
    const unsigned char input[]={0xc1,'\r','\n'};feed(input,sizeof(input));
    check_read((const unsigned char*)"A\n\r",3);
    mode(F_IGNCR|F_ICRNL,0,0);feed((const unsigned char*)"a\rb\n",4);
    check_read((const unsigned char*)"ab\n",3);
    struct kernel_tty_termios s;termios(&s);s.cflag&=~0x80U;settings(&s);
    feed((const unsigned char*)"Q",1);assert(read_into(result,1,KERNEL_FILES_O_NONBLOCK)==0);
}
static void status_flags(void) {
    mode(F_PARMRK|F_INPCK,0,0);flagged(255,0);flagged('X',4);flagged('Y',8);flagged('Z',16);flagged('Q',2);
    const unsigned char expected[]={255,255,255,0,'X',255,0,'Y',255,0,0,'Q'};
    check_read(expected,sizeof(expected));
    mode(F_IGNPAR|F_INPCK,0,0);flagged('X',4);flagged('Y',8);flagged('Z',16);flagged('Q',0);
    const unsigned char zero[]={0,'Q'};check_read(zero,2);
    mode(F_INPCK,0,0);flagged('X',4);flagged('Y',8);check_read((const unsigned char*)"\0\0",2);
    mode(F_PARMRK|F_ISTRIP,0,0);flagged(255,0);check_read((const unsigned char*)"\177",1);
    mode(F_IGNBRK|F_BRKINT,0,0);controlling();flagged('Z',16);assert(!signals[2]);
    assert(read_into(result,1,KERNEL_FILES_O_NONBLOCK)==0);
}
static void disabled_cc(void) {
    mode(F_IXON,0,L_ISIG|L_ICANON|L_IEXTEN);
    struct kernel_tty_termios s;termios(&s);
    unsigned indices[]={0,1,2,3,4,8,9,10,11,12,14,15,16};
    for(unsigned j=0;j<sizeof(indices)/sizeof(indices[0]);j++)s.cc[indices[j]]=0;
    settings(&s);controlling();const unsigned char bytes[]={0,'\n'};feed(bytes,2);check_read(bytes,2);
    assert(!signals[2]&&!signals[3]&&!signals[20]);
    write_text((const unsigned char*)"T",1);check_output((const unsigned char*)"T",1);
}
static void literal_next(void) {
    mode(F_ICRNL|F_IXON,0,L_ISIG|L_ICANON|L_IEXTEN);
    const unsigned char bytes[]={22,'\r',22,19,22,17,22,3,'\n'};
    feed(bytes,sizeof(bytes));const unsigned char want[]={'\r',19,17,3,'\n'};check_read(want,sizeof(want));
    write_text((const unsigned char*)"T",1);check_output((const unsigned char*)"T",1);
    mode(F_ISTRIP,0,L_ICANON|L_IEXTEN);const unsigned char strip[]={22,0xc1,'\n'};
    feed(strip,3);check_read((const unsigned char*)"A\n",2);
}
static void editing(void) {
    mode(0,0,L_ICANON|L_IEXTEN);
    feed((const unsigned char*)"one two  \027\n",11);check_read((const unsigned char*)"one \n",5);
    feed((const unsigned char*)"old\025new\n",8);check_read((const unsigned char*)"new\n",4);
    mode(F_IUTF8,0,L_ICANON|L_IEXTEN);
    const unsigned char utf8[]={'A',0xe4,0xb8,0xad,127,'B','\n'};
    feed(utf8,sizeof(utf8));check_read((const unsigned char*)"AB\n",3);
    mode(0,0,L_ICANON|L_IEXTEN);feed(utf8,sizeof(utf8));
    const unsigned char byte_erase[]={'A',0xe4,0xb8,'B','\n'};check_read(byte_erase,sizeof(byte_erase));
}
static void reprint_echo(void) {
    mode(0,5,L_ICANON|L_IEXTEN|L_ECHO|L_ECHOCTL|L_ECHOE|L_ECHOK|L_ECHOKE);
    feed((const unsigned char*)"ab\022\n",4);check_read((const unsigned char*)"ab\n",3);
    check_output((const unsigned char*)"ab^R\r\nab\r\n",10);
}
static void utf8_echo(void) {
    mode(F_IUTF8,0,L_ICANON|L_IEXTEN|L_ECHO|L_ECHOE);
    const unsigned char utf8[]={0xe4,0xb8,0xad,127,'\n'};feed(utf8,sizeof(utf8));
    check_read((const unsigned char*)"\n",1);
    const unsigned char expected[]={0xe4,0xb8,0xad,'\b',' ','\b','\n'};check_output(expected,sizeof(expected));
}
static void limits(void) {
    mode(0,0,L_ICANON|L_ISIG|L_IEXTEN);controlling();
    unsigned char line[4098];memset(line,'A',4095);line[4095]='B';line[4096]=127;line[4097]='C';feed(line,sizeof(line));
    feed((const unsigned char*)"\n",1);int64_t n=read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK);
    assert(n==4096&&result[4094]=='C'&&result[4095]=='\n');
    for(unsigned j=0;j<4094;j++)assert(result[j]=='A');
    memset(line,'X',4095);feed(line,4095);feed((const unsigned char*)"\003\n",2);
    assert(signals[2]==1);check_read((const unsigned char*)"\n",1);
    memset(line,'F',4095);feed(line,4095);feed((const unsigned char*)"\004",1);
    assert(read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK)==4095);
    feed((const unsigned char*)"Q\n",2);check_read((const unsigned char*)"Q\n",2);
    mode(0,0,0);memset(line,'R',sizeof(line));feed(line,sizeof(line));
    assert(read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK)==4095);
    for(unsigned j=0;j<4095;j++)assert(result[j]=='R');
}
static void signals_flush(void) {
    mode(0,0,L_ICANON|L_ISIG|L_NOFLSH);controlling();
    feed((const unsigned char*)"a\003b\n",4);assert(signals[2]==1);check_read((const unsigned char*)"ab\n",3);
    write_text((const unsigned char*)"OLD",3);flagged(3,0);check_output((const unsigned char*)"OLD",3);
    mode(F_BRKINT,0,L_ISIG|L_ICANON|L_NOFLSH);
    feed((const unsigned char*)"a",1);flagged(0,16);feed((const unsigned char*)"b\n",2);
    assert(signals[2]==3);check_read((const unsigned char*)"ab\n",3);
    mode(F_BRKINT,0,L_ISIG|L_ICANON);write_text((const unsigned char*)"OLD",3);
    feed((const unsigned char*)"a",1);flagged(0,16);feed((const unsigned char*)"b\n",2);
    check_read((const unsigned char*)"b\n",2);check_output((const unsigned char*)"",0);
    feed((const unsigned char*)"x\034y\032z\n",6);assert(signals[3]==1&&signals[20]==1);
    check_read((const unsigned char*)"z\n",2);
}
static void output_modes(void) {
    mode(0,1|4|8|16|0x1800,0);
    write_text((const unsigned char*)"\rA\r\n\tZ",6);
    /* ONOCR suppresses column-zero CR; OCRNL does not recursively ONLCR-expand. */
    check_output((const unsigned char*)"A\n\r\n        Z",13);
    mode(0,1|8|32,0);write_text((const unsigned char*)"AB\r\tX",5);
    check_output((const unsigned char*)"AB\n\tX",5);
}
static void flow(void) {
    mode(F_IXON,0,L_ICANON|L_IEXTEN);
    write_text((const unsigned char*)"A",1);feed((const unsigned char*)"\023",1);
    assert(kernel_tty_service_output(tty,256)==0);
    assert(!device->ioctl(instance,&task,0,0,0,0x540a,2));
    assert(kernel_tty_service_output(tty,256)==1&&output[0]==19);
    feed((const unsigned char*)"\021",1);check_output((const unsigned char*)"\023A",2);
    mode(F_IXON|F_IXANY,0,L_ICANON|L_IEXTEN);
    write_text((const unsigned char*)"B",1);feed((const unsigned char*)"\023Q\n",3);
    check_output((const unsigned char*)"B",1);check_read((const unsigned char*)"Q\n",2);
    mode(F_IXOFF,0,0);unsigned char many[4000];memset(many,'X',sizeof(many));feed(many,sizeof(many));
    assert(kernel_tty_service_output(tty,1)==1&&output[0]==19);
    assert(read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK)==4000);
    assert(kernel_tty_service_output(tty,1)==1&&output[1]==17);
    mode(F_IXON,0,0);write_text((const unsigned char*)"S",1);feed((const unsigned char*)"\023",1);
    kernel_tty_shutdown(tty);check_output((const unsigned char*)"S",1);
}
static void echo_atomic(void) {
    mode(0,0,L_ICANON|L_ISIG|L_NOFLSH|L_ECHO|L_ECHOCTL);
    unsigned char line[4095];memset(line,'A',sizeof(line));feed(line,sizeof(line));
    flagged(3,0);int32_t queued;assert(!ioctl_call(0x5411,&queued)&&queued==4095);
    credit=sizeof(output);assert(kernel_tty_service_output(tty,sizeof(output))==4095&&output_size==4095);
    for(size_t j=0;j<output_size;j++)assert(output[j]=='A');
}

static void flow_before_transform(void) {
    mode(F_IXON|F_ICRNL,0,L_ICANON);
    struct kernel_tty_termios s;termios(&s);s.cc[9]='\n';settings(&s);
    write_text((const unsigned char*)"T",1);feed((const unsigned char*)"\r",1);
    check_read((const unsigned char*)"\n",1);check_output((const unsigned char*)"T",1);
}
static void signal_before_transform(void) {
    mode(F_ICRNL,0,L_ISIG|L_ICANON);controlling();
    struct kernel_tty_termios s;termios(&s);s.cc[0]='\n';settings(&s);
    feed((const unsigned char*)"\r",1);assert(!signals[2]);check_read((const unsigned char*)"\n",1);
}
static void ixany_ignored_cr(void) {
    mode(F_IXON|F_IXANY|F_IGNCR,0,L_ICANON);
    write_text((const unsigned char*)"T",1);feed((const unsigned char*)"\023\r",2);
    check_output((const unsigned char*)"T",1);
}
/* Linux receive_buf_common reserves a complete three-byte PARMRK expansion
 * plus the canonical delimiter slot; parity records are ignored on overflow. */
static void parity_mark_limit(void) {
    mode(F_PARMRK|F_INPCK,0,L_ICANON);
    unsigned char line[4093];memset(line,'A',sizeof(line));feed(line,sizeof(line));
    flagged('P',4);feed((const unsigned char*)"\n",1);
    int64_t n=read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK);
    if(n!=4094)fprintf(stderr,"parity-mark-limit got=%lld expected=4094\n",(long long)n);
    assert(n==4094&&result[4093]=='\n');
    for(unsigned j=0;j<4093;j++)assert(result[j]=='A');
}

static void escaped_ff_limit(void) {
    mode(F_PARMRK,0,L_ICANON);
    unsigned char line[4094];memset(line,'A',sizeof(line));feed(line,sizeof(line));
    flagged(255,0);feed((const unsigned char*)"\n",1);
    assert(read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK)==4095);
    for(unsigned j=0;j<4094;j++)assert(result[j]=='A');
    assert(result[4094]=='\n');
    feed(line,4093);flagged(255,0);feed((const unsigned char*)"\n",1);
    assert(read_into(result,sizeof(result),KERNEL_FILES_O_NONBLOCK)==4096);
    for(unsigned j=0;j<4093;j++)assert(result[j]=='A');
    assert(result[4093]==255&&result[4094]==255&&result[4095]=='\n');
}
struct test_case {const char *name;void (*run)(void);};
static const struct test_case cases[]={
    {"transforms",transforms},{"status",status_flags},{"disabled-cc",disabled_cc},
    {"literal-next",literal_next},{"editing",editing},{"reprint",reprint_echo},
    {"utf8-echo",utf8_echo},{"limits",limits},{"signals",signals_flush},
    {"output",output_modes},{"flow",flow},{"echo-atomic",echo_atomic},
    {"flow-before-transform",flow_before_transform},{"signal-before-transform",signal_before_transform},
    {"ixany-ignored-cr",ixany_ignored_cr},{"parity-mark-limit",parity_mark_limit},{"escaped-ff-limit",escaped_ff_limit}};
int main(int argc,char **argv) {
    unsigned ran=0;
    for(unsigned j=0;j<sizeof(cases)/sizeof(cases[0]);j++) {
        if(argc>1&&strcmp(argv[1],cases[j].name))continue;
        start_case();cases[j].run();end_case();printf("TTY flags host: %s pass\n",cases[j].name);ran++;
    }
    assert(ran);return 0;
}
