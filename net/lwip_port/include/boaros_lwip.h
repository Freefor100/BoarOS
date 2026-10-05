#ifndef BOAROS_LWIP_HOOKS_H
#define BOAROS_LWIP_HOOKS_H
struct tcp_pcb;
struct boaros_lwip_hooks {
    void (*input)(struct tcp_pcb *);
    void (*work)(void);
    void (*capacity)(int);
};
void boaros_lwip_set_hooks(const struct boaros_lwip_hooks *hooks);
int boaros_lwip_tcp_input(struct tcp_pcb *pcb);
void boaros_lwip_work_ready(void);
void boaros_lwip_capacity_available(int pool);
#endif
