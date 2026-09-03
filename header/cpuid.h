#pragma once
#include <stdint.h>

void cpuid_vendor(char out[13]);   // 12-char vendor string + NUL
void cpuid_brand(char out[49]);    // up to 48-char brand string + NUL
