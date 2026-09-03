#pragma once
#include <stdint.h>
#include "header/net.h"

// Installs the UDP demultiplexer that both of these need, then runs a DHCP
// exchange. Returns 0 once an address is configured, -1 on timeout.
int  netcfg_init(uint32_t timeout_ms);

int  dhcp_run(uint32_t timeout_ms);

// Resolves a hostname to an IPv4 address, pumping the receive path while it
// waits. A dotted-quad is returned as-is without a query. Returns 0 on
// success.
int  dns_resolve(const char* host, ipv4_t* out, uint32_t timeout_ms);

// Small cache so revisiting a site does not re-query on every navigation.
void dns_cache_clear(void);
