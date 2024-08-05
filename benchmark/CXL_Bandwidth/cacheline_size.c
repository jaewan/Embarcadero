#include <stdio.h>
#include <stdint.h>

int main() {
    uint32_t eax, ebx, ecx, edx;
    eax = 0x01; // Cache and TLB Information
    __asm__("cpuid"
            : "=a" (eax), "=b" (ebx), "=c" (ecx), "=d" (edx)
            : "a" (eax));
    printf("Cache Line Size: %d bytes\n", (ebx >> 8) & 0xff);
    return 0;
}
