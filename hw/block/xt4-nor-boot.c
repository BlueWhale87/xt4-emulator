/* SPDX-License-Identifier: GPL-2.0-or-later
 * Named virtual MX25UM51245GXDI00 boot-command subset.
 * Public part protocol does not establish the camera's fitted device.
 */
#include "qemu/osdep.h"
#include "hw/block/xt4-nor-boot.h"

static bool failure(const char **detail, const char *reason)
{
    *detail = reason;
    return false;
}

void xt4_nor_boot_reset(XT4NorState *s)
{
    s->protocol = XT4_NOR_SPI;
    s->write_enabled = false;
    s->generation++;
}

static bool no_address_data(const XT4NorRequest *r)
{
    return !r->address_bytes && !r->dummy_cycles && !r->tx_len && !r->rx_len;
}

bool xt4_nor_boot_prepare(const XT4NorState *s, const XT4NorRequest *r,
                         XT4NorEffect *effect, const char **detail)
{
    if (r->tx_len > sizeof(r->tx) || r->rx_len > sizeof(effect->rx)) {
        return failure(detail, "unsupported_nor_length");
    }
    if (r->protocol != s->protocol ||
        (r->protocol == XT4_NOR_SPI && r->has_extension) ||
        (r->protocol == XT4_NOR_OPI_DTR &&
         (!r->has_extension || r->extension != (uint8_t)(r->opcode ^ 0xff))) ||
        (r->protocol != XT4_NOR_SPI && r->protocol != XT4_NOR_OPI_DTR)) {
        return failure(detail, "unsupported_nor_protocol");
    }
    XT4NorEffect result = {
        .expected_generation = s->generation,
        .next_protocol = s->protocol,
        .next_write_enabled = s->write_enabled,
    };
    switch (r->opcode) {
    case 0x9f: {
        static const uint8_t id[] = {0xc2, 0x80, 0x3a};
        if (r->protocol != XT4_NOR_SPI || r->address_bytes ||
            r->dummy_cycles || r->tx_len || !r->rx_len || r->rx_len > sizeof(id)) {
            return failure(detail, "unsupported_nor_identification");
        }
        memcpy(result.rx, id, r->rx_len);
        result.rx_len = r->rx_len;
        break;
    }
    case 0x06:
    case 0x04:
        if (!no_address_data(r)) {
            return failure(detail, "unsupported_nor_write_enable_frame");
        }
        result.next_write_enabled = r->opcode == 0x06;
        break;
    case 0x05:
        if (r->tx_len || !r->rx_len ||
            (r->protocol == XT4_NOR_SPI &&
             (r->dummy_cycles ||
              (r->address_bytes && (r->address_bytes != 4 || r->address)))) ||
            (r->protocol == XT4_NOR_OPI_DTR &&
             (r->address_bytes != 4 || r->address || r->dummy_cycles != 4 ||
              r->rx_len & 1))) {
            return failure(detail, "unsupported_nor_status_frame");
        }
        /* Status is a stream. Extra zero address clocks in the admitted SPI
         * frame discard earlier status samples; they do not name a register.
         * OPI DTR repeats each byte, which is identical throughout this stream.
         * No program/erase is supported, so there is no pending WIP state. */
        memset(result.rx, s->write_enabled ? 2 : 0, r->rx_len);
        result.rx_len = r->rx_len;
        break;
    case 0x72:
        if (r->protocol != XT4_NOR_SPI || r->address_bytes != 4 || r->address ||
            r->dummy_cycles || r->tx_len != 1 || r->tx[0] != 2 || r->rx_len) {
            return failure(detail, "unsupported_nor_configuration");
        }
        if (s->write_enabled) {
            result.next_protocol = XT4_NOR_OPI_DTR;
            result.next_write_enabled = false;
            /* Published typical volatile WRCR2 duration, not a maximum or
             * a claim about the camera's fitted part or controller timing. */
            result.settle_ns = 40;
        } else {
            result.ignored_write = true;
        }
        break;
    default:
        return failure(detail, "unsupported_nor_command");
    }
    result.state_change = result.next_protocol != s->protocol ||
                          result.next_write_enabled != s->write_enabled;
    *effect = result;
    *detail = NULL;
    return true;
}

bool xt4_nor_boot_commit(XT4NorState *s, const XT4NorEffect *effect,
                        const char **detail)
{
    if (s->generation != effect->expected_generation) {
        return failure(detail, "stale_nor_effect");
    }
    s->protocol = effect->next_protocol;
    s->write_enabled = effect->next_write_enabled;
    s->generation++;
    *detail = NULL;
    return true;
}
