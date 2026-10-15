/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_BLOCK_XT4_NOR_BOOT_H
#define HW_BLOCK_XT4_NOR_BOOT_H

/* A named virtual MX25UM51245GXDI00 boot-command subset, not a camera ID. */
typedef enum XT4NorProtocol {
    XT4_NOR_SPI,
    XT4_NOR_OPI_DTR,
} XT4NorProtocol;

typedef struct XT4NorRequest {
    XT4NorProtocol protocol;
    uint8_t opcode, extension;
    bool has_extension;
    uint32_t address;
    uint8_t address_bytes, dummy_cycles, tx_len, rx_len;
    uint8_t tx[8];
} XT4NorRequest;

typedef struct XT4NorState {
    XT4NorProtocol protocol;
    bool write_enabled;
    uint64_t generation;
} XT4NorState;

typedef struct XT4NorEffect {
    uint64_t expected_generation;
    XT4NorProtocol next_protocol;
    bool next_write_enabled, state_change, ignored_write;
    uint8_t rx[8], rx_len;
    uint64_t settle_ns;
} XT4NorEffect;

/* Zero-initialize before first reset. No host I/O, timer, QOM or media access. */
void xt4_nor_boot_reset(XT4NorState *s);
bool xt4_nor_boot_prepare(const XT4NorState *s, const XT4NorRequest *request,
                         XT4NorEffect *effect, const char **detail);
bool xt4_nor_boot_commit(XT4NorState *s, const XT4NorEffect *effect,
                        const char **detail);
#endif
