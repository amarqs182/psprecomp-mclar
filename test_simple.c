
#include "psprecomp/vfpu.h"
#include <stdio.h>
#include <string.h>

int main(void) {
    printf("Testing vi2uc with proper input values\n");
    psp_mem_init();
    psp_cpu_reset();
    psp_vfpu_reset();
    
    // Set up test data with values where bits 23-30 are non-trivial
    // For byte value 0x12 in position i, we need (value >> 23) & 0xFF = 0x12
    // So we put 0x12 << 23 in that lane
    int s[4], d[4];
    psp_vfpu_regs(0x04, 4, s);
    psp_vfpu_regs(0x08, 1, d);
    
    // Lane 0: byte 0 = 0x12 -> need 0x12 << 23 = 0x12000000
    // But we want to test the pack, so let's use known patterns
    // Byte 0 -> lane 0:  byte_pattern << 23
    uint32_t in_data[4] = {
        0x12000000u,  // byte 0 = 0x12
        0x34000000u,  // byte 1 = 0x34
        0x56000000u,  // byte 2 = 0x56
        0x78000000u   // byte 3 = 0x78
    };
    for (int i = 0; i < 4; i++) memcpy(&psp_cpu.v[s[i]], &in_data[i], 4);
    
    psp_vpack(PSP_VP_I2UC, 0x08, 0x04, 4);
    
    uint32_t got;
    memcpy(&got, &psp_cpu.v[d[0]], 4);
    printf("Result: 0x%08X (expected 0x78563412)\n", got);
    
    // Test with signed values too
    uint32_t in_data2[4] = {
        0x000000FFu,  // byte 0 = 0xFF, positive
        0x00000100u,  // byte 1 = 0x01
        0x00000200u,  // byte 2 = 0x02
        0xF0000000u   // signed negative, should clamp to 0
    };
    for (int i = 0; i < 4; i++) memcpy(&psp_cpu.v[s[i]], &in_data2[i], 4);
    
    psp_vpack(PSP_VP_I2UC, 0x08, 0x04, 4);
    
    memcpy(&got, &psp_cpu.v[d[0]], 4);
    printf("Result2: 0x%08X (expected 0x020100FF with negative clamped to 0)\n", got);
    
    psp_mem_free();
    return 0;
}
