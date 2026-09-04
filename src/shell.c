#include <stdint.h>
#include "header/kprintf.h"
#include "header/tty.h"
#include "header/console.h"
#include "header/pmm.h"
#include "header/kheap.h"
#include "header/cpuid.h"
#include "header/rtc.h"
#include "header/fat32.h"
#include "header/desktop.h"
#include "header/shell.h"
#include "header/task.h"
#include "header/net.h"
#include "header/netcfg.h"
#include "header/http.h"

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

static void cmd_ls(void){
    if (!fs_ready) { kprintf("no filesystem mounted (no disk attached, or not FAT32)\n"); return; }
    fat32_dirent_t entries[32];
    int n = fat32_list(cur_dir, entries, 32);
    if (n < 0) { kprintf("ls: read error\n"); return; }
    for (int i = 0; i < n; i++)
        kprintf("%s%s\n", entries[i].name, entries[i].is_dir ? "/" : "");
}

static void cmd_cd(const char* name){
    if (!fs_ready) { kprintf("no filesystem mounted\n"); return; }
    fat32_dirent_t e;
    if (fat32_find(cur_dir, name, &e) != 0) { kprintf("cd: not found: %s\n", name); return; }
    if (!e.is_dir) { kprintf("cd: not a directory: %s\n", name); return; }
    cur_dir = e.first_cluster ? e.first_cluster : fat32_root_cluster();
}

#define CAT_MAX (32u * 1024u)

static void cmd_cat(const char* name){
    if (!fs_ready) { kprintf("no filesystem mounted\n"); return; }
    fat32_dirent_t e;
    if (fat32_find(cur_dir, name, &e) != 0) { kprintf("cat: not found: %s\n", name); return; }
    if (e.is_dir) { kprintf("cat: is a directory: %s\n", name); return; }
    uint32_t cap = e.size < CAT_MAX ? e.size : CAT_MAX;
    uint8_t* buf = (uint8_t*)kmalloc(cap + 1);
    if (!buf) { kprintf("cat: out of memory\n"); return; }
    uint32_t got = fat32_read_file(&e, buf, cap);
    buf[got] = 0;
    kprintf("%s", (const char*)buf);
    if (got < e.size) kprintf("\n[...truncated, file is %u bytes]\n", e.size);
    else if (got > 0 && buf[got - 1] != '\n') kprintf("\n");
    kfree(buf);
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

void shell_init(void){
    if (fs_ready) return;
    if (fat32_init() == 0){ fs_ready = 1; cur_dir = fat32_root_cluster(); }
}

int      shell_fs_ready(void){ return fs_ready; }
uint32_t shell_cwd_cluster(void){ return cur_dir; }

void shell_print_help(void){
    kprintf("help | echo <text> | uptime | meminfo | cpuinfo | date | clear | crash\n");
    kprintf("ls | cd <dir> | cat <file> | gui\n");
    kprintf("net | dhcp | dns <host> | ping <host>\n");
    kprintf("fetch <url> | get <url>\n");
    kprintf("ps | spawn | kill <id>\n");
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
        cmd_ls();
    }else if(starts(b,"cd ")){
        cmd_cd(b+3);
    }else if(starts(b,"cat ")){
        cmd_cat(b+4);
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
