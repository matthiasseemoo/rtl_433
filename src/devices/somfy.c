/** @file
    Somfy RTS.

    Copyright (C) 2020 Matthias Schulz <mschulz@seemoo.tu-darmstadt.de>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.
*/

/**
Somfy RTS.

Protocol description:
The protocol is very well defined under the following two links:
[1] https://pushstack.wordpress.com/somfy-rts-protocol/
[2] https://patentimages.storage.googleapis.com/bd/ae/4f/bf24e41e0161ca/US8189620.pdf

Each frame consists of a preamble with hardware and software sync pulses followed by the manchester encoded data pulses.
A rising edge describes a data bit 1 and a falling edge a data bit 0. The preamble is different for the first frame and
for retransmissions. In the end, the signal is first decoded using an OOK PCM decoder and within the callback, only the
data bits will be manchester decoded.

In the following, each character representing a low level "_" and a high level "^" is roughly 604 us long.

First frames' preamble:

    ^^^^^^^^^^^^^^^^___________^^^^____^^^^____^^^^^^^^_

The first long pulse is often wrongly detected, so I just make sure that it ends up in another row during decoding and
then only consider the rows containing the second part of the first frame preamble.

Retransmission frames' preamble:

    ^^^^____^^^^____^^^^____^^^^____^^^^____^^^^____^^^^____^^^^^^^^_

The data is manchester encoded _^ represents a 1 and ^_ represents a 0. The data section consists of 56 bits that equals
7 bytes of scrambled data. The data is scrambled by XORing each following byte with the last scrambled byte. After
descrambling, the 7 bytes have the following meaning conting byte from left to right as in big endian byte order:

- byte 0:   called "random" in [1] and "key" in [2], in the end it is just the seed for the scrambler
- byte 1:   The higher nibble represents the control command, the lower nibble is the frame's checksum calculated by XORing
            all nibbles
- byte 2-3: Replay counter value in big endian byte order
- byte 4-6: Remote control channel's address
*/

#include "decoder.h"

static const char *control_str[] = {
    "? (0)",
    "My (1)",
    "Up (2)",
    "My + Up (3)",
    "Down (4)",
    "My + Down (5)",
    "Up + Down (6)",
    "? (7)",
    "Prog (8)",
    "Sun + Flag (9)",
    "Flag (10)",
    "? (11)",
    "? (12)",
    "? (13)",
    "? (14)",
    "? (15)",
};

static int somfy_rts_callback(r_device *decoder, bitbuffer_t *bitbuffer)
{
    data_t *data;

    uint8_t is_retransmission = 0;
    uint8_t decode_row = 0;
    uint8_t data_start = 0;
    const uint8_t *preamble_pattern;
    unsigned preamble_pattern_bit_length = 0;

    bitbuffer_t bitbuffer_decoded = { 0 };

    uint8_t message_bytes[7] = { 0 };
    uint8_t chksum_calc = 0;
    uint8_t chksum = 0;
    uint16_t counter = 0;
    char address_hex[7];
    uint32_t address_int_le = 0;
    uint8_t control = 0;
    uint8_t seed = 0;

    for (int i = 0; i < bitbuffer->num_rows; i++) {
        if (bitbuffer->bits_per_row[i] > 170) {
            is_retransmission = 1;
            decode_row = i;
            data_start = 65;
            preamble_pattern = (const uint8_t *) "\xf0\xf0\xf0\xf0\xf0\xf0\xf0\xff";
            preamble_pattern_bit_length = 64;
            break;
        } else if (bitbuffer->bits_per_row[i] > 130) {
            is_retransmission = 0;
            decode_row = i;
            data_start = 25;
            preamble_pattern = (const uint8_t *) "\xf0\xf0\xff";
            preamble_pattern_bit_length = 24;
            break;
        }
    }

    if (data_start == 0)
        return DECODE_FAIL_SANITY;

    if (bitbuffer_search(bitbuffer, decode_row, 0, preamble_pattern, preamble_pattern_bit_length) != 0)
        return DECODE_FAIL_SANITY;

    if (bitbuffer_manchester_decode(bitbuffer, decode_row, data_start, &bitbuffer_decoded, 56) - data_start < 56)
        return DECODE_FAIL_MIC;

    bitbuffer_extract_bytes(&bitbuffer_decoded, 0, 0, message_bytes, 56);

    // descramble
    for (int i = 6; i > 0; i--)
        message_bytes[i] = message_bytes[i] ^ message_bytes[i-1];

    // calculate checksum
    chksum_calc = xor_bytes(message_bytes, 7);
    chksum_calc = (chksum_calc & 0xf) ^ (chksum_calc >> 4); // fold to nibble

    // Fail if checksum is incorrect
    if (chksum_calc != 0)
        return DECODE_FAIL_MIC;

    // extract seed
    seed = message_bytes[0];

    // extract control
    control = (message_bytes[1] & 0xf0) >> 4;

    // extract checksum
    chksum = message_bytes[1] & 0xf;

    // extract counter independent of system's byte order
    counter = message_bytes[3] | (message_bytes[2] << 8);

    // extract address bytes into hexstring
    snprintf(address_hex, sizeof(address_hex), "%02X%02X%02X", message_bytes[4], message_bytes[5], message_bytes[6]);

    // extract address as little endian integer. It should be little endian as multiple addresses used by one remote control increase the address value in little endian byte order.
    address_int_le = (message_bytes[6] << 16) | (message_bytes[5] << 8) | message_bytes[4];

    if (decoder->verbose > 1) {
        fprintf(stderr, "seed=0x%02x, chksum=0x%x\n", seed, chksum);
    }

    /* clang-format off */
    data = data_make(
            "model",          "",               DATA_STRING, "Somfy-RTS",
            "id",             "Id",             DATA_INT,    address_int_le,
            "control",        "Control",        DATA_STRING, control_str[control],
            "counter",        "Counter",        DATA_INT,    counter,
            "address",        "Address",        DATA_STRING, address_hex,
            "retransmission", "Retransmission", DATA_STRING, is_retransmission ? "TRUE" : "FALSE",
            "mic",            "Integrity",      DATA_STRING, "CHECKSUM",
            NULL);
    /* clang-format on */

    decoder_output_data(decoder, data);

    return 1;
}

static char *output_fields[] = {
        "model",
        "control",
        "counter",
        "address",
        "retransmission",
        NULL,
};

// rtl_433 -r g001_433.414M_250k.cu8 -X "n=somfy-test,m=OOK_PCM,s=604,l=604,t=40,r=10000,g=3000,y=2416"
// Nominal bit width is ~604 us, RZ, short=long

r_device somfy_rts = {
        .name           = "Somfy RTS",
        .modulation     = OOK_PULSE_PCM_RZ,
        .short_width    = 604,   // short pulse is ~604 us (1 x nominal bit width)
        .long_width     = 604,   // long pulse is ~604 us (1 x nominal bit width)
//        .sync_width     = 2416,  // hardware sync pulse is ~2416 us (4 x nominal bit width), software sync pulse is ~4550 us. Commented, as sync_width has no effect on the PCM decoder.
        .gap_limit      = 3000,  // largest off between two pulses is ~2416 us during sync. Gap between start pulse (9664 us) and first frame is 6644 us (11 x nominal bit width), 3000 us will split first message into two rows one with start pulse and one with first frame
        .reset_limit    = 10000, // larger than gap between start pulse and first frame (6644 us = 11 x nominal bit width) to put start pulse and first frame in two rows, but smaller than inter-frame space of 30415 us
        .tolerance      = 20,
        .decode_fn      = &somfy_rts_callback,
        .disabled       = 0,
        .fields         = output_fields,
};
