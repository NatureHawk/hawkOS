#pragma once
#include <stdint.h>

#define ETH_MTU      1514      // 14-byte header + 1500 payload, no CRC
#define ETH_ALEN     6

// Driver for the Realtek RTL8139, the NIC QEMU emulates most faithfully.
//
// Receive is polled rather than interrupt driven. The card's IRQ line is
// assigned by the PCI bus at runtime, so an interrupt path would mean
// installing a handler for a vector that is not known until boot; polling
// from the network task costs one port read per loop and removes that whole
// moving part. The scheduler makes it cheap — the task sleeps between polls
// instead of spinning.
int  rtl8139_init(void);
int  rtl8139_present(void);

const uint8_t* rtl8139_mac(void);

// Queues one frame for transmission. Returns 0 on success, -1 if all four
// transmit descriptors are still busy.
int  rtl8139_send(const void* frame, uint16_t len);

// Pulls the next received frame into buf. Returns its length, or 0 if the
// receive ring is empty.
uint16_t rtl8139_poll(uint8_t* buf, uint16_t cap);
