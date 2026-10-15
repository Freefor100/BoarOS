/* 立即退出故意让root收口与尚在执行I/O的setup任务交错；
 * 实际CPU0/CPU1组合另由原静态及pthread ELF验证。 */
.section .text
.global _start
_start:
    li a0, 42
    li a7, 93
    ecall
1:  j 1b
