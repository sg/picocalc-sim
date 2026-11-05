// A self-checking test of the RP2350's Arm security extension, which none of the firmware the CI
// boots uses: Secure code configures the SAU and calls Non-secure code, which calls back through
// secure gateways, takes exceptions in both states and provokes the SecureFaults the architecture
// defines, with each check printed to UART0.
#include <stdbool.h>
#include <stdint.h>

#define REG(a) (*(volatile uint32_t *)(a))
#define NS_REG(a) REG((a) + 0x20000)
#define NSDATA __attribute__((section(".ns_data")))
#define NSTEXT __attribute__((section(".ns_text"), noinline))

// As link.ld lays out the image.
#define S_ADDR 0x20000040u
#define NSC_BASE 0x20002000u
#define NS_BASE 0x20002020u
#define NS_LIMIT 0x20007fffu
#define NS_STACK_TOP 0x20008000u

#define UART0_DR 0x40070000u
#define UART0_FR 0x40070018u
#define UART0_CR 0x40070030u
#define RESETS_CLR 0x40023000u
#define RESETS_DONE 0x40020008u
#define ACCESSCTRL_GPIO_NSMASK0 0x4006000cu
#define SIO_GPIO_OUT 0xd0000010u
#define SIO_GPIO_OUT_SET 0xd0000018u
#define SIO_GPIO_OE_SET 0xd0000020u
#define SIO_FIFO_ST 0xd0000050u
#define SIO_FIFO_WR 0xd0000054u
#define SIO_FIFO_RD 0xd0000058u
#define SIO_SPINLOCK_ST 0xd000005cu
#define SIO_SPINLOCK5 0xd0000114u
#define SIO_DOORBELL_IN_SET 0xd0000188u
#define SYST_CSR 0xe000e010u
#define SYST_RVR 0xe000e014u
#define SYST_CVR 0xe000e018u
#define NVIC_ISER0 0xe000e100u
#define NVIC_ISER1 0xe000e104u
#define NVIC_ISPR1 0xe000e204u
#define NVIC_ITNS0 0xe000e380u
#define NVIC_ITNS1 0xe000e384u
#define ICSR 0xe000ed04u
#define VTOR 0xe000ed08u
#define SHCSR 0xe000ed24u
#define CFSR 0xe000ed28u
#define HFSR 0xe000ed2cu
#define CPACR 0xe000ed88u
#define NSACR 0xe000ed8cu
#define SAU_CTRL 0xe000edd0u
#define SAU_RNR 0xe000edd8u
#define SAU_RBAR 0xe000eddcu
#define SAU_RLAR 0xe000ede0u
#define SFSR 0xe000ede4u
#define SFAR 0xe000ede8u

#define ACCESSCTRL 0x40060000u
#define AC_PASSWORD 0xacce0000u
#define AC_LOCK (ACCESSCTRL + 0x00)
#define AC_FORCE_CORE_NS (ACCESSCTRL + 0x04)
#define AC_CFGRESET (ACCESSCTRL + 0x08)
#define AC_RESETS (ACCESSCTRL + 0x64)
#define AC_IO_BANK0 (ACCESSCTRL + 0x68)
#define AC_SPI0 (ACCESSCTRL + 0x90)
#define AC_UART0 (ACCESSCTRL + 0xa0)
#define IO_BANK0_GPIO_CTRL(n) (0x40028004u + 8u * (n))
#define RESETS_RESET 0x40020000u
#define RESETS_SET (RESETS_RESET + 0x2000u)
#define TIMER0_TIMERAWL 0x400b0028u
#define BOOTRAM 0x400e0000u
#define SPI0_SSPCR0 0x40080000u
#define CORE1_STACK_TOP 0x20041000u
#define DMA_CH(n) (0x50000000u + 0x40u * (n))
#define DMA_SECCFG_CH(n) (0x50000480u + 4u * (n))

#define IRQ_NS_TEST 46
#define IRQ_S_TEST 47
#define IRQ_SIO_BELL_NS 28

NSDATA volatile uint32_t sf_sfsr, sf_sfar, sf_count, sf_from_ns, bf_count;
NSDATA volatile uint32_t systick_from_ns, ns_step, ns_msp, ns_pendsv_count, ns_unexpected;
NSDATA volatile uint32_t ns_irq_regs = 0xffffffffu, ns_irq_ipsr, ns_irq_lr;
NSDATA volatile uint32_t ns_handler_ipsr, ns_handler_sg_ipsr;
NSDATA volatile uint32_t ns_bell_seen, ns_lock_claim, ns_lock_st, ns_gpio_out, ns_dcp_x;
NSDATA volatile uint32_t ns_uart_cr, ns_gpio3_ctrl, ns_gpio5_ctrl, ns_resets;
NSDATA volatile uint32_t dma_scratch;

uint32_t resume_to;
volatile uint32_t s_irq_ipsr_after, s_irq_ns_result;
volatile uint32_t core1_go, core1_lock_st, core1_faults, core1_done;
static unsigned passes, fails;

extern int sg_add(int a, int b);
extern uint32_t sg_ipsr(void);
extern uint32_t call_ns(void *fn, uint32_t arg);
extern uint32_t pend_and_check(uint32_t word, uint32_t bits);
extern void s_bad_transition(uint32_t target);
extern uint32_t ns_load(uint32_t a);
extern void ns_store(uint32_t a, uint32_t v);
extern void s_store(uint32_t a, uint32_t v);
extern uint32_t s_load(uint32_t a);
extern void ns_jump(uint32_t a);
extern void s_add(void);
extern uint32_t ns_vectors[];

static void putc_(char ch)
{
    while (REG(UART0_FR) & (1u << 5))
        ;
    REG(UART0_DR) = (uint8_t)ch;
}

static void puts_(const char *s)
{
    while (*s)
        putc_(*s++);
}

static void puthex(uint32_t v)
{
    puts_("0x");
    for (int i = 28; i >= 0; i -= 4)
        putc_("0123456789abcdef"[(v >> i) & 15]);
}

static void putdec(unsigned v)
{
    char buf[12], *p = buf + sizeof(buf) - 1;
    *p = 0;
    do
        *--p = (char)('0' + v % 10);
    while (v /= 10);
    puts_(p);
}

static void check(const char *what, uint32_t got, uint32_t want)
{
    puts_(got == want ? "ok   " : "FAIL ");
    puts_(what);
    puts_(" = ");
    puthex(got);
    if (got != want) {
        puts_(" want ");
        puthex(want);
        fails++;
    } else {
        passes++;
    }
    putc_('\n');
}

void s_unexpected(uint32_t ipsr)
{
    puts_("UNEXPECTED exception ");
    putdec(ipsr);
    puts_(" SFSR ");
    puthex(REG(SFSR));
    puts_(" CFSR ");
    puthex(REG(CFSR));
    puts_(" HFSR ");
    puthex(REG(HFSR));
    putc_('\n');
    for (;;)
        ;
}

// BusFaults resume at the faulting leaf function's return address, as do Non-secure code's
// SecureFaults; Secure code's resume where the test asked.
void s_on_fault(uint32_t *frame, uint32_t exc_return)
{
    uint32_t ipsr;
    __asm volatile("mrs %0, ipsr" : "=r"(ipsr));
    bool from_secure = exc_return & 0x40;
    if (ipsr == 5) {
        bf_count++;
        REG(CFSR) = 0xff00;
        frame[6] = frame[5] & ~1u;
        return;
    }
    sf_sfsr = REG(SFSR);
    sf_sfar = REG(SFAR);
    REG(SFSR) = 0xff;
    sf_count++;
    sf_from_ns = !from_secure;
    frame[6] = (from_secure ? resume_to : frame[5]) & ~1u;
}

void s_on_systick(uint32_t exc_return)
{
    if (!(exc_return & 0x40)) {
        systick_from_ns++;
        REG(SYST_CSR) = 0;
    }
}

static inline uint32_t tt(uint32_t a)
{
    uint32_t r;
    __asm volatile("tt %0, %1" : "=r"(r) : "r"(a));
    return r;
}

static inline uint32_t tta(uint32_t a)
{
    uint32_t r;
    __asm volatile("tta %0, %1" : "=r"(r) : "r"(a));
    return r;
}

// ------------------------------------------------------------------ Non-secure side

NSTEXT static uint32_t ns_ipsr(void)
{
    uint32_t r;
    __asm volatile("mrs %0, ipsr" : "=r"(r));
    return r;
}

// Returns 0x600d, or the step it failed at.
NSTEXT uint32_t ns_main(uint32_t x)
{
    ns_step = 1;
    if (sg_add((int)x, 5) != (int)x + 5)
        return 1;
    ns_step = 2;
    if (sg_ipsr() != 0)
        return 2;
    ns_step = 3;
    REG(ICSR) = 1u << 28;
    __asm volatile("dsb\n isb" ::: "memory");
    if (ns_pendsv_count != 1)
        return 3;
    ns_step = 4;
    ns_load(S_ADDR);
    if (sf_count != 1 || sf_sfar != S_ADDR)
        return 4;
    ns_step = 5;
    ns_jump((uint32_t)&s_add);
    if (sf_count != 2)
        return 5;
    ns_step = 6;
    for (uint32_t i = 0; i < 1000000 && !systick_from_ns; i++)
        ;
    if (!systick_from_ns)
        return 6;
    ns_step = 7;
    __asm volatile("mrs %0, msp" : "=r"(ns_msp));
    if (NS_REG(VTOR) != 0)
        return 7;
    return 0x600d;
}

NSTEXT uint32_t ns_sio(uint32_t x)
{
    ns_lock_claim = REG(SIO_SPINLOCK5);
    ns_lock_st = REG(SIO_SPINLOCK_ST);
    REG(SIO_GPIO_OUT_SET) = 0xff;
    ns_gpio_out = REG(SIO_GPIO_OUT);
    REG(NVIC_ISER0) = 1u << IRQ_SIO_BELL_NS;
    REG(SIO_DOORBELL_IN_SET) = 0x5;
    __asm volatile("dsb\n isb\n nop\n nop" ::: "memory");
    ns_load(SIO_SPINLOCK_ST + 0x20000);
    return x;
}

NSTEXT uint32_t ns_dcp(uint32_t x)
{
    uint32_t lo, hi;
    __asm volatile("mcrr p5, #0, %0, %1, c0" ::"r"(x), "r"(0x5555u));
    __asm volatile("mrrc p5, #0, %0, %1, c8" : "=r"(lo), "=r"(hi));
    ns_dcp_x = lo;
    return hi;
}

// Peripherals Non-secure code reaches only once Secure code grants them, and only the GPIOs,
// resets and permissions that grant covers.
NSTEXT uint32_t ns_peripherals(uint32_t granted)
{
    ns_uart_cr = ns_load(UART0_CR);
    if (!granted)
        return 0;
    ns_store(AC_UART0, AC_PASSWORD | 0x03);
    ns_store(ACCESSCTRL_GPIO_NSMASK0, 0xffffffffu);
    ns_gpio5_ctrl = ns_load(IO_BANK0_GPIO_CTRL(5));
    ns_store(IO_BANK0_GPIO_CTRL(5), 2);
    ns_store(IO_BANK0_GPIO_CTRL(3), 5);
    ns_gpio3_ctrl = ns_load(IO_BANK0_GPIO_CTRL(3));
    ns_resets = ns_load(RESETS_RESET);
    ns_store(RESETS_SET, 1u << 18);
    ns_load(BOOTRAM);
    return 0;
}

NSTEXT uint32_t ns_from_handler(uint32_t x)
{
    ns_handler_ipsr = ns_ipsr();
    ns_handler_sg_ipsr = sg_ipsr();
    return x + 1;
}

// ------------------------------------------------------------------ Secure side

void s_on_irq(uint32_t ipsr)
{
    if (ipsr != 16 + IRQ_S_TEST)
        s_unexpected(ipsr);
    s_irq_ns_result = call_ns(ns_from_handler, 41);
    uint32_t r;
    __asm volatile("mrs %0, ipsr" : "=r"(r));
    s_irq_ipsr_after = r;
}

static void sau_region(unsigned n, uint32_t base, uint32_t limit, bool nsc)
{
    REG(SAU_RNR) = n;
    REG(SAU_RBAR) = base;
    REG(SAU_RLAR) = (limit & ~0x1fu) | (nsc ? 2 : 0) | 1;
}

static void test_attribution(void)
{
    sau_region(0, NS_BASE, NS_LIMIT, false);
    sau_region(1, NSC_BASE, NSC_BASE + 0x1f, true);
    REG(SAU_CTRL) = 1;
    __asm volatile("dsb\n isb" ::: "memory");
    check("TT of Secure SRAM: S, no SAU region", tt(S_ADDR) & 0x00460000, 0x00440000);
    check("TT of Non-secure SRAM", tt(NS_BASE + 0x100) & 0x007fffff, 0x003e0000);
    check("TT of a gateway: S, SAU region 1", tt(NSC_BASE + 4) & 0x007fffff, 0x004e0100);
    check("TT of an IDAU-exempt peripheral", tt(UART0_DR), 0x004c0000);
    check("TTA of Non-secure SRAM", tta(NS_BASE + 0x100) & 0x003c0000, 0x003c0000);
    NS_REG(VTOR) = (uint32_t)ns_vectors;
    check("VTOR_NS through the alias", NS_REG(VTOR), (uint32_t)ns_vectors);
    check("VTOR_S untouched", REG(VTOR), 0x20000000);
}

static void test_ns_call(void)
{
    __asm volatile("msr msp_ns, %0" ::"r"(NS_STACK_TOP));
    REG(SYST_RVR) = 3000;
    REG(SYST_CVR) = 0;
    REG(SYST_CSR) = 7;
    uint32_t r = call_ns(ns_main, 7);
    REG(SYST_CSR) = 0;
    check("Non-secure call result", r, 0x600d);
    check("Non-secure steps reached", ns_step, 7);
    check("Non-secure PendSV taken", ns_pendsv_count, 1);
    check("SecureFaults taken", sf_count, 2);
    check("last SecureFault was INVEP", sf_sfsr, 1);
    check("Secure SysTick preempted Non-secure code", systick_from_ns, 1);
    check("MSP as Non-secure code saw it", ns_msp >= NS_STACK_TOP - 0x100 && ns_msp < NS_STACK_TOP, 1);
    uint32_t msp_ns;
    __asm volatile("mrs %0, msp_ns" : "=r"(msp_ns));
    check("MSP_NS balanced", msp_ns, NS_STACK_TOP);
}

static void test_exceptions(void)
{
    REG(NVIC_ITNS1) = 1u << (IRQ_NS_TEST - 32);
    REG(NVIC_ISER1) = 1u << (IRQ_NS_TEST - 32);
    check("callee registers restored", pend_and_check(1, 1u << (IRQ_NS_TEST - 32)), 0);
    check("Non-secure handler saw cleared registers", ns_irq_regs, 0);
    check("Non-secure handler IPSR", ns_irq_ipsr, 16 + IRQ_NS_TEST);
    check("Non-secure handler EXC_RETURN", ns_irq_lr, 0xfffffff8);

    s_bad_transition(NS_BASE + 0x100);
    check("INVTRAN SecureFault", sf_sfsr, 1u << 4);
    check("INVTRAN from Secure state", sf_from_ns, 0);

    REG(NVIC_ISER1) = 1u << (IRQ_S_TEST - 32);
    REG(NVIC_ISPR1) = 1u << (IRQ_S_TEST - 32);
    __asm volatile("dsb\n isb\n nop\n nop" ::: "memory");
    check("Non-secure result in a Secure handler", s_irq_ns_result, 42);
    check("Non-secure IPSR under a Secure handler", ns_handler_ipsr, 1);
    check("gateway IPSR under a Secure handler", ns_handler_sg_ipsr, 1);
    check("IPSR after FNC_RETURN", s_irq_ipsr_after, 16 + IRQ_S_TEST);
}

static void test_sio(void)
{
    REG(SHCSR) |= 1u << 17;
    REG(ACCESSCTRL_GPIO_NSMASK0) = 1u << 3;
    REG(NVIC_ITNS0) = 1u << IRQ_SIO_BELL_NS;
    REG(SIO_GPIO_OE_SET) = 0xff;
    uint32_t s_claim = REG(SIO_SPINLOCK5);
    call_ns(ns_sio, 0);
    check("Secure spinlock 5 claimed", s_claim != 0, 1);
    check("Non-secure spinlock 5 claimed too", ns_lock_claim != 0, 1);
    check("Non-secure spinlock state", ns_lock_st, 1u << 5);
    check("Non-secure bank through the mirror", NS_REG(SIO_SPINLOCK_ST), 1u << 5);
    check("Secure spinlock state", REG(SIO_SPINLOCK_ST), 1u << 5);
    REG(SIO_SPINLOCK5) = 1;
    NS_REG(SIO_SPINLOCK5) = 1;
    check("spinlocks released", REG(SIO_SPINLOCK_ST) | NS_REG(SIO_SPINLOCK_ST), 0);
    check("Non-secure GPIO write masked", REG(SIO_GPIO_OUT), 1u << 3);
    check("Non-secure GPIO read masked", ns_gpio_out, 1u << 3);
    check("Non-secure doorbell interrupt", ns_bell_seen, 0x5);
    check("Secure doorbells untouched", REG(SIO_DOORBELL_IN_SET), 0);
    check("Non-secure use of the mirror is a BusFault", bf_count, 1);
}

static void test_dcp(void)
{
    REG(CPACR) |= 0xfu << 8;
    REG(NSACR) |= 1u << 5;
    NS_REG(CPACR) |= 3u << 10;
    __asm volatile("mcrr p4, #0, %0, %1, c0" ::"r"(0x1234u), "r"(0x4444u));
    uint32_t ns_hi = call_ns(ns_dcp, 0x9876u);
    uint32_t lo, hi;
    __asm volatile("mrrc p4, #0, %0, %1, c8" : "=r"(lo), "=r"(hi));
    check("Non-secure DCP (coprocessor 5)", ns_dcp_x | (ns_hi << 16), 0x55559876u);
    check("Secure DCP (coprocessor 4) untouched", lo | (hi << 16), 0x44441234u);
}

static uint32_t dma_copy(unsigned ch, uint32_t seccfg, uint32_t src)
{
    REG(DMA_SECCFG_CH(ch)) = seccfg;
    REG(DMA_CH(ch) + 0x00) = src;
    REG(DMA_CH(ch) + 0x04) = (uint32_t)&dma_scratch;
    REG(DMA_CH(ch) + 0x08) = 1;
    REG(DMA_CH(ch) + 0x0c) = 1 | (2u << 2) | (ch << 13) | (0x3fu << 17) | (1u << 23);
    while (REG(DMA_CH(ch) + 0x0c) & (1u << 26))
        ;
    return REG(DMA_CH(ch) + 0x0c) & 0xe0000000u;
}

// Core 1 stays in Secure state, but with FORCE_CORE_NS set the bus sees it as Non-secure.
void core1_main(void)
{
    REG(SHCSR) |= 1u << 17;
    while (!core1_go)
        ;
    core1_lock_st = REG(SIO_SPINLOCK_ST);
    unsigned before = bf_count;
    s_load(SPI0_SSPCR0);
    core1_faults = bf_count - before;
    core1_done = 1;
    for (;;)
        __asm volatile("wfe");
}

// The boot ROM's core 1 launch handshake, as the SDK's multicore_launch_core1_raw does it, which
// must happen before FORCE_CORE_NS gives core 1 the other bank's FIFOs.
static void launch_core1(void (*entry)(void))
{
    const uint32_t cmds[6] = {0, 0, 1, (uint32_t)0x20000000, CORE1_STACK_TOP, (uint32_t)entry};
    unsigned i = 0;
    while (i < 6) {
        uint32_t cmd = cmds[i];
        if (!cmd) {
            while (REG(SIO_FIFO_ST) & 1)
                (void)REG(SIO_FIFO_RD);
            __asm volatile("sev");
        }
        while (!(REG(SIO_FIFO_ST) & 2))
            ;
        REG(SIO_FIFO_WR) = cmd;
        __asm volatile("sev");
        while (!(REG(SIO_FIFO_ST) & 1))
            __asm volatile("wfe");
        i = REG(SIO_FIFO_RD) == cmd ? i + 1 : 0;
    }
}

static void test_force_core_ns(void)
{
    launch_core1(core1_main);
    (void)REG(SIO_SPINLOCK5 + 4);
    (void)NS_REG(SIO_SPINLOCK5 + 8);
    REG(AC_FORCE_CORE_NS) = AC_PASSWORD | 2;
    core1_go = 1;
    while (!core1_done)
        ;
    check("FORCE_CORE_NS gives core 1 the Non-secure SIO bank", core1_lock_st, 1u << 7);
    check("FORCE_CORE_NS keeps core 1 from Secure-only peripherals", core1_faults, 1);
    REG(SIO_SPINLOCK5 + 4) = 1;
    NS_REG(SIO_SPINLOCK5 + 8) = 1;
    REG(AC_FORCE_CORE_NS) = AC_PASSWORD;
}

static void test_accessctrl(void)
{
    unsigned faults = bf_count;
    call_ns(ns_peripherals, 0);
    check("Non-secure access to a Secure-only peripheral is a BusFault", bf_count - faults, 1);
    s_store(AC_SPI0, 0xfe);
    check("ACCESSCTRL write without the password is a BusFault", bf_count - faults, 2);
    check("  and changes nothing", REG(AC_SPI0), 0xfc);

    REG(AC_UART0) = AC_PASSWORD | 0xfe;
    REG(AC_IO_BANK0) = AC_PASSWORD | 0xfe;
    REG(AC_RESETS) = AC_PASSWORD | 0xfe;
    REG(IO_BANK0_GPIO_CTRL(5)) = 5;
    REG(RESETS_CLR) = 1u << 18;
    faults = bf_count;
    call_ns(ns_peripherals, 1);
    check("granted peripheral read by Non-secure code", ns_uart_cr, 0x301);
    check("Non-secure code may grant its own unprivileged code", REG(AC_UART0), 0xff);
    check("Non-secure write to GPIO_NSMASK0 ignored", REG(ACCESSCTRL_GPIO_NSMASK0), 1u << 3);
    check("Secure-only GPIO reads as zero to Non-secure code", ns_gpio5_ctrl, 0);
    check("Secure-only GPIO ignores Non-secure writes", REG(IO_BANK0_GPIO_CTRL(5)), 5);
    check("Non-secure GPIO written by Non-secure code", ns_gpio3_ctrl, 5);
    uint32_t ns_view = (1u << 6) | (1u << 21) | (1u << 26);
    check("RESETS shows Non-secure code its own peripherals", ns_resets, REG(RESETS_RESET) & ns_view);
    check("RESETS ignores Non-secure resets of Secure-only ones", REG(RESETS_RESET) & (1u << 18), 0);
    check("boot RAM is a BusFault to Non-secure code", bf_count - faults, 1);

    REG(IO_BANK0_GPIO_CTRL(3)) = 1;
    check("Secure-only SPI0 can't be selected on a Non-secure GPIO", REG(IO_BANK0_GPIO_CTRL(3)) & 0x1f, 0x1f);
    REG(IO_BANK0_GPIO_CTRL(3)) = 2;
    check("granted UART0 can", REG(IO_BANK0_GPIO_CTRL(3)) & 0x1f, 2);
    REG(IO_BANK0_GPIO_CTRL(6)) = 1;
    REG(ACCESSCTRL_GPIO_NSMASK0) = (1u << 3) | (1u << 6);
    check("granting a GPIO deselects a Secure-only function", REG(IO_BANK0_GPIO_CTRL(6)) & 0x1f, 0x1f);

    check("Secure DMA channel reads a Secure-only timer", dma_copy(0, 3, TIMER0_TIMERAWL), 0);
    check("  and its level locks", REG(DMA_SECCFG_CH(0)), 7);
    check("Non-secure DMA channel reads SRAM", dma_copy(1, 1, (uint32_t)&ns_step), 0);
    check("Non-secure DMA channel can't read the timer", dma_copy(2, 1, TIMER0_TIMERAWL), 0xc0000000u);

    REG(AC_LOCK) = AC_PASSWORD | 1;
    REG(AC_UART0) = AC_PASSWORD | 0xfc;
    REG(AC_CFGRESET) = AC_PASSWORD | 1;
    check("LOCK holds core 0's DMA and its own bits", REG(AC_LOCK), 5);
    check("  and makes core 0's writes no-ops", REG(AC_UART0), 0xff);
}

void s_main(void)
{
    REG(RESETS_CLR) = 1u << 26;
    while (!(REG(RESETS_DONE) & (1u << 26)))
        ;
    REG(UART0_CR) = 0x301;
    puts_("TrustZone test\n");
    REG(SHCSR) |= 1u << 19;
    test_attribution();
    test_ns_call();
    test_exceptions();
    test_sio();
    test_dcp();
    test_force_core_ns();
    test_accessctrl();
    check("no unexpected Non-secure exception", ns_unexpected, 0);
    puts_(fails ? "TZ FAIL: " : "TZ PASS: ");
    putdec(passes);
    puts_(" passed, ");
    putdec(fails);
    puts_(" failed\n");
    for (;;)
        __asm volatile("wfi");
}
