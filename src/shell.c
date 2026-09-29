#include <stdint.h>
#include "header/kprintf.h"
#include "header/tty.h"
#include "header/console.h"
#include "header/pmm.h"
#include "header/kheap.h"
#include "header/cpuid.h"
#include "header/rtc.h"
#include "header/fat32.h"
#include "header/vfs.h"
#include "header/desktop.h"
#include "header/shell.h"
#include "header/task.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/http.h"
#include "header/acpi.h"
#include "header/apic.h"
#include "header/ata.h"
#include "header/proc.h"

extern volatile unsigned long long ticks;

static int fs_ready = 0;
static uint32_t cur_dir = 0;

static int streq(const char* a,const char* b){
    while(*a||*b){ if(*a!=*b) return 0; a++; b++; } return 1;
}
static int starts(const char* s,const char* p){
    while(*p){ if(*s++!=*p++) return 0; } return 1;
}

static void cmd_meminfo(void){
    uint32_t total_kb  = pmm_total_kb();
    uint32_t used_kb   = pmm_used_kb();
    uint32_t free_kb   = total_kb - used_kb;

    size_t heap_used = 0, heap_free = 0;
    kheap_stats(&heap_used, &heap_free);

    kprintf("physical: %u KB total, %u KB used, %u KB free\n", total_kb, used_kb, free_kb);
    kprintf("kheap:    %u KB used, %u KB free\n",
            (unsigned)(heap_used / 1024u), (unsigned)(heap_free / 1024u));
}

static void cmd_cpuinfo(void){
    char vendor[13];
    char brand[49];
    cpuid_vendor(vendor);
    cpuid_brand(brand);
    kprintf("vendor: %s\n", vendor);
    kprintf("brand:  %s\n", brand);
}

static void put2(unsigned v){   // zero-padded 2-digit
    kprintf("%c%c", (char)('0' + (v / 10) % 10), (char)('0' + v % 10));
}

static void cmd_date(void){
    rtc_time_t t;
    rtc_read(&t);
    kprintf("%u-", t.year);
    put2(t.month); kprintf("-"); put2(t.day);
    kprintf(" ");
    put2(t.hour); kprintf(":"); put2(t.min); kprintf(":"); put2(t.sec);
    kprintf(" (UTC, from CMOS RTC)\n");
}

static void cmd_crash(void){
    kprintf("[shell] triggering a divide-by-zero on purpose...\n");
    kprintf("[shell] the exception handler should catch it below, then the kernel halts.\n");
    __asm__ __volatile__("int $0x0");
}

// The shell's working directory is a path string resolved by the VFS, so `ls`,
// `cd` and `cat` also work on /dev and on ".." / "." components. cur_dir
// mirrors it as a FAT32 cluster for the Files app (see shell_cwd_cluster).
static char cwd[VFS_PATH_MAX] = "/";

static void cmd_ls(const char* arg){
    if (!fs_ready) { kprintf("no filesystem mounted (no disk attached, or not FAT32)\n"); return; }
    vfs_file_t* d;
    int r = vfs_open_file(cwd, arg && *arg ? arg : ".", VFS_O_RDONLY, &d);
    if (r < 0) { kprintf("ls: cannot open (error %d)\n", r); return; }
    if (d->type != VFS_T_DIR) { kprintf("ls: not a directory\n"); vfs_file_put(d); return; }
    vfs_dirent_t e;
    while (vfs_file_readdir(d, &e) == 1)
        kprintf("%s%s\n", e.name, e.type == VFS_T_DIR ? "/" : "");
    vfs_file_put(d);
}

static void cmd_cd(const char* name){
    if (!fs_ready) { kprintf("no filesystem mounted\n"); return; }
    int r = vfs_chdir(cwd, name && *name ? name : "/");
    if (r == VFS_ENOENT) { kprintf("cd: not found: %s\n", name); return; }
    if (r == VFS_ENOTDIR) { kprintf("cd: not a directory: %s\n", name); return; }
    if (r < 0) { kprintf("cd: error %d\n", r); return; }
    vfs_stat_t st;
    if (vfs_stat(cwd, ".", &st) == 0 && st.dev == 1)
        cur_dir = st.ino ? st.ino : fat32_root_cluster();
}

#define CAT_MAX (32u * 1024u)

static void cmd_cat(const char* name){
    if (!fs_ready) { kprintf("no filesystem mounted\n"); return; }
    vfs_file_t* f;
    int r = vfs_open_file(cwd, name, VFS_O_RDONLY, &f);
    if (r == VFS_ENOENT) { kprintf("cat: not found: %s\n", name); return; }
    if (r < 0) { kprintf("cat: error %d\n", r); return; }
    if (f->type == VFS_T_DIR) { kprintf("cat: is a directory: %s\n", name); vfs_file_put(f); return; }

    vfs_stat_t st;
    vfs_file_fstat(f, &st);
    uint32_t size = st.size;
    uint32_t cap = size < CAT_MAX ? size : CAT_MAX;
    uint8_t* buf = (uint8_t*)kmalloc(cap + 1);
    if (!buf) { kprintf("cat: out of memory\n"); vfs_file_put(f); return; }
    uint32_t got = 0;
    while (got < cap) {
        int n = vfs_file_read(f, buf + got, cap - got);
        if (n <= 0) break;
        got += (uint32_t)n;
    }
    vfs_file_put(f);
    buf[got] = 0;
    kprintf("%s", (const char*)buf);
    if (got < size) kprintf("\n[...truncated, file is %u bytes]\n", size);
    else if (got > 0 && buf[got - 1] != '\n') kprintf("\n");
    kfree(buf);
}

// Runs a user program from the shell's working directory, waits for it and
// reports how it ended. Its output goes to the console like the shell's own.
static void cmd_run(const char* arg){
    while (*arg == ' ') arg++;
    if (!*arg){ kprintf("usage: run <program> [args]\n"); return; }
    if (!fs_ready){ kprintf("no filesystem mounted\n"); return; }

    char prog[VFS_PATH_MAX];
    uint32_t n = 0;
    while (arg[n] && arg[n] != ' ' && n < sizeof(prog) - 1){ prog[n] = arg[n]; n++; }
    prog[n] = 0;
    const char* args = arg + n;

    int pid = proc_spawn_ex(prog, args, cwd, 0);
    if (pid < 0){ kprintf("run: cannot start %s\n", prog); return; }
    int code = 0;
    if (proc_wait(pid, &code) < 0) { kprintf("run: lost track of pid %d\n", pid); return; }
    kprintf("[%s exited with %d]\n", prog, code);
}

static int parse_uint(const char* s, uint32_t* out){
    uint32_t v = 0; int any = 0;
    while (*s == ' ') s++;
    while (*s >= '0' && *s <= '9'){ v = v * 10u + (uint32_t)(*s - '0'); s++; any = 1; }
    if (!any) return -1;
    *out = v;
    return 0;
}

// Sleeps between prints so it is visibly a background task rather than a
// burst of output: `ps` run from another window shows it cycling through
// sleeping and ready while it lives.
static void worker_task(void* arg){
    unsigned n = (unsigned)(uint32_t)arg;
    for (unsigned i = 0; i < 5; i++){
        kprintf("[worker %u] tick %u\n", n, i);
        task_sleep(500);
    }
    kprintf("[worker %u] finished\n", n);
}

static void cmd_net(void){
    if (!net_link_up()){ kprintf("no network interface\n"); return; }

    const uint8_t* m = net_mac();
    kprintf("mac:     %02X:%02X:%02X:%02X:%02X:%02X\n", m[0], m[1], m[2], m[3], m[4], m[5]);

    if (!net_configured()){ kprintf("address: not configured (DHCP did not complete)\n"); return; }

    char b[16];
    kprintf("ip:      %s\n", net_ip_str(net_local_ip(), b));
    kprintf("mask:    %s\n", net_ip_str(net_netmask(), b));
    kprintf("gateway: %s\n", net_ip_str(net_gateway(), b));
    kprintf("dns:     %s\n", net_ip_str(net_dns_server(), b));
}

static void cmd_ping(const char* host){
    ipv4_t ip;
    if (dns_resolve(host, &ip, 4000) != 0){ kprintf("ping: cannot resolve %s\n", host); return; }

    char b[16];
    kprintf("pinging %s (%s)\n", host, net_ip_str(ip, b));

    int got = 0;
    for (uint16_t seq = 1; seq <= 4; seq++){
        uint32_t before = icmp_reply_count();
        icmp_ping(ip, seq);
        for (int w = 0; w < 60; w++){
            net_poll();
            if (icmp_reply_count() > before){ got++; break; }
            task_sleep(10);
        }
    }
    kprintf("%d of 4 replies\n", got);
}

static void cmd_dns(const char* host){
    ipv4_t ip;
    char b[16];
    if (dns_resolve(host, &ip, 5000) == 0) kprintf("%s -> %s\n", host, net_ip_str(ip, b));
    else                                   kprintf("dns: lookup failed for %s\n", host);
}

static void cmd_dhcp(void){
    kprintf("requesting a lease...\n");
    if (dhcp_run(8000) == 0) cmd_net();
    else                     kprintf("dhcp: no reply\n");
}

static void fetch_progress(void* ctx, const char* stage){
    (void)ctx;
    kprintf("  %s\n", stage);
}

static void cmd_fetch(const char* url, int show_body){
    http_response_t r;
    if (http_get(url, &r, fetch_progress, 0) != 0){
        kprintf("fetch: %s\n", r.error[0] ? r.error : "failed");
        return;
    }

    kprintf("status %d  %s  %u bytes\n", r.status,
            r.content_type[0] ? r.content_type : "(no content-type)", r.body_len);
    kprintf("final url: %s\n", r.final_url);

    if (show_body){
        // Only the first slice: a real page would scroll the whole terminal
        // away, and this command exists to prove the transfer worked.
        uint32_t n = r.body_len < 400u ? r.body_len : 400u;
        for (uint32_t i = 0; i < n; i++){
            char c = (char)r.body[i];
            kprintf("%c", (c == '\n' || (c >= 32 && c <= 126)) ? c : '.');
        }
        kprintf("\n");
    }
    http_response_free(&r);
}

static void cmd_acpi(void){
    const acpi_info_t* a = acpi_info();
    if (!a->valid){ kprintf("acpi: not available\n"); return; }
    kprintf("rsdp:   0x%x rev %u oem '%s' checksum %s\n", (unsigned)a->rsdp_addr,
            (unsigned)a->rsdp_rev, a->oem, a->rsdp_ok ? "ok" : "BAD");
    kprintf("root:   %s at 0x%x, %d tables, checksum %s\n", a->used_xsdt ? "XSDT" : "RSDT",
            (unsigned)a->root_addr, a->ntables, a->root_ok ? "ok" : "BAD");
    kprintf("fadt:   rev %u checksum %s, PM1a_CNT 0x%x PM1b_CNT 0x%x, SCI_EN %d\n",
            (unsigned)a->fadt_rev, a->fadt_ok ? "ok" : "BAD",
            (unsigned)a->pm1a_cnt, (unsigned)a->pm1b_cnt, a->sci_en);
    if (a->s5_found) kprintf("_S5:    SLP_TYPa=%u SLP_TYPb=%u\n", (unsigned)a->slp_typa, (unsigned)a->slp_typb);
    else             kprintf("_S5:    not found\n");
    kprintf("reset:  %s\n", a->have_reset_reg ? (a->reset_space ? "FADT I/O register" : "FADT memory register") : "8042 pulse only");
    kprintf("madt:   LAPIC 0x%x, %d CPU(s), %d IOAPIC(s)\n", (unsigned)a->lapic_addr, a->ncpus, a->nioapics);
    for (int i = 0; i < a->ncpus; i++)
        kprintf("  cpu%d apic id %u %s\n", i, (unsigned)a->cpus[i].apic_id,
                a->cpus[i].enabled ? "enabled" : "disabled");
    for (int i = 0; i < a->nioapics; i++)
        kprintf("  ioapic %u at 0x%x gsi %u\n", (unsigned)a->ioapics[i].id,
                (unsigned)a->ioapics[i].addr, (unsigned)a->ioapics[i].gsi_base);
}

static void cmd_lapic(const char* arg){
    if (!apic_ready()){ kprintf("lapic: not available\n"); return; }
    if (streq(arg, "on")){
        if (apic_timer_start(100) != 0){ kprintf("lapic: cannot start timer\n"); return; }
        // Prove it: count both tick sources over the same half second.
        unsigned long long p0 = ticks; uint32_t l0 = apic_tick_count();
        task_sleep(500);
        unsigned long long p1 = ticks; uint32_t l1 = apic_tick_count();
        kprintf("lapic timer on at 100 Hz: %u LAPIC ticks vs %u PIT ticks in 500 ms\n",
                (unsigned)(l1 - l0), (unsigned)(p1 - p0));
        kprintf("(the PIT still drives uptime and the scheduler)\n");
        return;
    }
    if (streq(arg, "off")){ apic_timer_stop(); kprintf("lapic timer off\n"); return; }
    kprintf("lapic id %u ver 0x%x at 0x%x, timer clock %u Hz\n", (unsigned)apic_id(),
            (unsigned)apic_version(), (unsigned)apic_base_addr(), (unsigned)apic_timer_hz());
    kprintf("timer: %s", apic_timer_running() ? "running" : "off");
    if (apic_timer_running()) kprintf(" at %u Hz", (unsigned)apic_timer_target_hz());
    kprintf(", %u LAPIC ticks (usage: lapic [on|off])\n", (unsigned)apic_tick_count());
}

// Times raw sector reads through each engine. Read-only, 64 sectors at a time
// from the start of the disk, so it is safe on a live image.
static uint32_t bench_raw(int dma, uint32_t sectors, uint8_t* buf){
    unsigned long long t0 = ticks;
    for (uint32_t lba = 0; lba < sectors; lba += 64){
        int r = dma ? ata_read_sectors_dma(lba, 64, buf) : ata_read_sectors_pio(lba, 64, buf);
        if (r != 0) return 0xFFFFFFFFu;
    }
    return (uint32_t)((ticks - t0) * 10u);
}

static void cmd_ata(const char* arg){
    if (streq(arg, "dma on"))  { ata_dma_enable(1); }
    else if (streq(arg, "dma off")) { ata_dma_enable(0); }
    else if (starts(arg, "bench")){
        uint8_t* buf = (uint8_t*)kmalloc(64 * 512);
        if (!buf){ kprintf("ata: out of memory\n"); return; }
        const uint32_t n = 8192;                       // 4 MB
        uint32_t ms = bench_raw(0, n, buf);
        kprintf("raw PIO read of %u KB: %u ms\n", n / 2, ms);
        if (ata_dma_available()){
            ms = bench_raw(1, n, buf);
            kprintf("raw DMA read of %u KB: %u ms\n", n / 2, ms);
        }
        kfree(buf);
        if (arg[5] == ' ' && arg[6] && fs_ready){
            fat32_dirent_t e;
            if (fat32_find(cur_dir, arg + 6, &e) != 0 || e.is_dir){ kprintf("ata: no such file: %s\n", arg + 6); return; }
            uint32_t cap = e.size < (8u << 20) ? e.size : (8u << 20);
            uint8_t* fb = (uint8_t*)kmalloc(cap ? cap : 1);
            if (!fb){ kprintf("ata: out of memory\n"); return; }
            int was = ata_dma_enabled();
            for (int pass = 0; pass < 2; pass++){
                if (pass && !ata_dma_available()) break;
                ata_dma_enable(pass);
                unsigned long long t0 = ticks;
                uint32_t got = fat32_read_file(&e, fb, cap);
                kprintf("file read of %u KB via %s: %u ms\n", got / 1024u, pass ? "DMA" : "PIO",
                        (unsigned)((ticks - t0) * 10u));
            }
            ata_dma_enable(was);
            kfree(fb);
        }
        return;
    }
    ata_stats_t st;
    ata_stats(&st);
    kprintf("ata: dma %s (%s); %u dma xfers, %u pio xfers, %u fallbacks\n",
            ata_dma_available() ? "available" : "unavailable",
            ata_dma_enabled() ? "on" : "off",
            (unsigned)st.dma_ops, (unsigned)st.pio_ops, (unsigned)st.fallbacks);
    kprintf("usage: ata [dma on|dma off|bench [file]]\n");
}

void shell_init(void){
    if (fs_ready) return;
    if (fat32_init() == 0){ fs_ready = 1; cur_dir = fat32_root_cluster(); }
}

int      shell_fs_ready(void){ return fs_ready; }
uint32_t shell_cwd_cluster(void){ return cur_dir; }

void shell_print_help(void){
    kprintf("help | echo <text> | uptime | meminfo | cpuinfo | date | clear | crash\n");
    kprintf("ls [dir] | cd <dir> | cat <file> | run <program> [args] | gui\n");
    kprintf("net | dhcp | dns <host> | ping <host>\n");
    kprintf("fetch <url> | get <url>\n");
    kprintf("ps | spawn | kill <id>\n");
    kprintf("poweroff | reboot | acpi | lapic [on|off] | ata [dma on|off|bench [file]]\n");
}

void shell_exec_line(const char* b){
    if (!b || !b[0]) return;

    if(streq(b,"help")){
        shell_print_help();
    }else if(starts(b,"echo ")){
        kprintf("%s\n", b+5);
    }else if(streq(b,"uptime")){
        unsigned long long t = ticks;
        unsigned s = (unsigned)(t/100);
        unsigned ms = (unsigned)(t%100)*10;
        kprintf("%us %ums\n", s, ms);
    }else if(streq(b,"meminfo")){
        cmd_meminfo();
    }else if(streq(b,"cpuinfo")){
        cmd_cpuinfo();
    }else if(streq(b,"date") || streq(b,"time")){
        cmd_date();
    }else if(streq(b,"clear")){
        console_init();
    }else if(streq(b,"crash")){
        cmd_crash();
    }else if(streq(b,"ls")){
        cmd_ls(0);
    }else if(starts(b,"ls ")){
        cmd_ls(b+3);
    }else if(starts(b,"cd ")){
        cmd_cd(b+3);
    }else if(starts(b,"cat ")){
        cmd_cat(b+4);
    }else if(starts(b,"run ")){
        cmd_run(b+4);
    }else if(streq(b,"ps")){
        task_ps();
    }else if(streq(b,"spawn")){
        static unsigned seq = 1;
        int id = task_create("worker", worker_task, (void*)(uint32_t)seq);
        if (id < 0) kprintf("spawn: no free task slot\n");
        else        kprintf("spawned worker %u\n", seq);
        seq++;
    }else if(starts(b,"kill ")){
        uint32_t id;
        if (parse_uint(b+5, &id) != 0) { kprintf("kill: usage: kill <id>\n"); return; }
        int r = task_kill(id);
        if      (r == -2) kprintf("kill: task %u is not killable\n", id);
        else if (r == -1) kprintf("kill: no such task: %u\n", id);
        else              kprintf("killed task %u\n", id);
    }else if(streq(b,"net")){
        cmd_net();
    }else if(streq(b,"dhcp")){
        cmd_dhcp();
    }else if(starts(b,"dns ")){
        cmd_dns(b+4);
    }else if(starts(b,"ping ")){
        cmd_ping(b+5);
    }else if(starts(b,"fetch ")){
        cmd_fetch(b+6, 0);
    }else if(starts(b,"get ")){
        cmd_fetch(b+4, 1);
    }else if(streq(b,"poweroff") || streq(b,"shutdown")){
        acpi_poweroff();
    }else if(streq(b,"reboot")){
        acpi_reboot();
    }else if(streq(b,"acpi")){
        cmd_acpi();
    }else if(streq(b,"lapic")){
        cmd_lapic("");
    }else if(starts(b,"lapic ")){
        cmd_lapic(b+6);
    }else if(streq(b,"ata")){
        cmd_ata("");
    }else if(starts(b,"ata ")){
        cmd_ata(b+4);
    }else if(streq(b,"gui")){
        desktop_run();
    }else{
        kprintf("unknown: %s\n", b);
    }
}

void shell_run(void){
    char b[128];

    shell_init();

    kprintf("[shell] type 'help'\n");
    for(;;){
        int n = tty_readline("> ", b, sizeof(b));
        if(n<=0) continue;
        shell_exec_line(b);
    }
}
