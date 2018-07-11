/* RadioHead ASK (generic) protocol
 *
 * Default transmitter speed is 2000 bits per second, i.e. 500 us per bit.
 */

#include "rtl_433.h"
#include "pulse_demod.h"
#include "util.h"

// Maximum message length (including the headers, byte count and FCS) we are willing to support
// This is pretty arbitrary
#define RH_ASK_MAX_PAYLOAD_LEN 67
#define RH_ASK_HEADER_LEN 4
#define RH_ASK_MAX_MESSAGE_LEN (RH_ASK_MAX_PAYLOAD_LEN - RH_ASK_HEADER_LEN - 3)

uint8_t rh_payload[RH_ASK_MAX_PAYLOAD_LEN] = {0};
int rh_data_payload[RH_ASK_MAX_MESSAGE_LEN];

// Transmitter speed in bits per seconds
#define SL_ASK_SPEED 1000
#define SL_ASK_BIT_LEN (int)1e6/SL_ASK_SPEED

// Maximum message length (including the headers, byte count and FCS) we are willing to support
// This is pretty arbitrary
#define SL_ASK_MAX_PAYLOAD_LEN 80
#define SL_ASK_HEADER_LEN 4
#ifndef SL_ASK_MAX_MESSAGE_LEN
    #define SL_ASK_MAX_MESSAGE_LEN (SL_ASK_MAX_PAYLOAD_LEN - SL_ASK_HEADER_LEN - 3)
#endif

uint8_t sl_payload[SL_ASK_MAX_PAYLOAD_LEN] = {0};

// Note: all tje "4to6 code" came from RadioHead source code.
// see: http://www.airspayce.com/mikem/arduino/RadioHead/index.html

// 4 bit to 6 bit symbol converter table
// Used to convert the high and low nybbles of the transmitted data
// into 6 bit symbols for transmission. Each 6-bit symbol has 3 1s and 3 0s
// with at most 3 consecutive identical bits
static uint8_t symbols[] = {
        0x0d, 0x0e, 0x13, 0x15, 0x16, 0x19, 0x1a, 0x1c,
        0x23, 0x25, 0x26, 0x29, 0x2a, 0x2c, 0x32, 0x34
};

// Convert a 6 bit encoded symbol into its 4 bit decoded equivalent
static uint8_t symbol_6to4(uint8_t symbol)
{
    uint8_t i;
    // Linear search :-( Could have a 64 byte reverse lookup table?
    // There is a little speedup here courtesy Ralph Doncaster:
    // The shortcut works because bit 5 of the symbol is 1 for the last 8
    // symbols, and it is 0 for the first 8.
    // So we only have to search half the table
    for (i = (symbol >> 2) & 8; i < 16; i++) {
        if (symbol == symbols[i])
            return i;
    }
    return 0xFF; // Not found
}

static int radiohead_ask_callback(bitbuffer_t *bitbuffer)
{
    // Get time
    char time_str[LOCAL_TIME_BUFLEN];
    data_t *data;

    uint8_t row = 0; // we are considering only first row
    unsigned int len = bitbuffer->bits_per_row[row];

    uint8_t msg_len = RH_ASK_MAX_MESSAGE_LEN;
    unsigned int pos, nb_bytes;
    uint8_t rxBits[2] = {0};

    uint16_t crc, crc_recompute;
    uint8_t data_len, header_to, header_from, header_id, header_flags;

    // Looking for preamble
    uint8_t init_pattern[] = {
            0x55, // 8
            0x55, // 16
            0x55, // 24
            0x51, // 32
            0xcd, // 40
    };
    // The first 0 is ignored by the decoder, so we look only for 28 bits of "01"
    // and not 32. Also "0x1CD" is 0xb38 (RH_ASK_START_SYMBOL) with LSBit first.
    uint8_t init_pattern_len = 40;

    pos = bitbuffer_search(bitbuffer, row, 0, init_pattern, init_pattern_len);
    if (pos == len) {
        if (debug_output) {
            printf("RH ASK preamble not found\n");
        }
        return 0;
    }

    // read "bytes" of 12 bit
    nb_bytes = 0;
    pos += init_pattern_len;
    for (; pos < len && nb_bytes < msg_len; pos += 12) {
        bitbuffer_extract_bytes(bitbuffer, row, pos, rxBits, /*len=*/16);
        // ^ we should read 16 bits and not 12, elsewhere last 4bits are ignored
        rxBits[0] = reverse8(rxBits[0]);
        rxBits[1] = reverse8(rxBits[1]);
        rxBits[1] = ((rxBits[1] & 0x0F) << 2) + (rxBits[0] >> 6);
        rxBits[0] &= 0x3F;
        uint8_t hi_nibble = symbol_6to4(rxBits[0]);
        if (hi_nibble > 0xF) {
            if (debug_output) {
                fprintf(stdout, "Error on 6to4 decoding high nibble: %X\n", rxBits[0]);
            }
            return 0;
        }
        uint8_t lo_nibble = symbol_6to4(rxBits[1]);
        if (lo_nibble > 0xF) {
            if (debug_output) {
                fprintf(stdout, "Error on 6to4 decoding low nibble: %X\n", rxBits[1]);
            }
            return 0;
        }
        uint8_t byte = hi_nibble << 4 | lo_nibble;
        rh_payload[nb_bytes] = byte;
        if (nb_bytes == 0) {
            msg_len = byte;
        }
        nb_bytes++;
    }

    // Get header
    data_len = msg_len - RH_ASK_HEADER_LEN - 3;
    header_to = rh_payload[1];
    header_from = rh_payload[2];
    header_id = rh_payload[3];
    header_flags = rh_payload[4];

    // Check CRC
    crc = rh_payload[5 + data_len] + (rh_payload[5 + data_len + 1] << 8);
    crc_recompute = ~crc16(rh_payload, msg_len - 2, 0x8408, 0xFFFF);
    if (crc_recompute != crc) {
        if (debug_output) {
            fprintf(stdout, "CRC error: %04X != %04X\n", crc_recompute, crc);
        }
        return 0;
    }

    // Format data
    for (int j = 0; j < msg_len; j++) {
        rh_data_payload[j] = (int)rh_payload[5 + j];
    }
    local_time_str(0, time_str);
    data = data_make(
            "time",         "",             DATA_STRING, time_str,
            "model",        "",             DATA_STRING, "RadioHead ASK",
            "len",          "Data len",     DATA_INT, data_len,
            "to",           "To",           DATA_INT, header_to,
            "from",         "From",         DATA_INT, header_from,
            "id",           "Id",           DATA_INT, header_id,
            "flags",        "Flags",        DATA_INT, header_flags,
            "payload",      "Payload",      DATA_ARRAY, data_array(data_len, DATA_INT, rh_data_payload),
            "mic",          "Integrity",    DATA_STRING, "CRC",
            NULL);
    data_acquired_handler(data);

    return 1;
}

static int sensible_living_callback(bitbuffer_t *bitbuffer)
{
    // Get time
    char time_str[LOCAL_TIME_BUFLEN];
    data_t *data;
    local_time_str(0, time_str);

    uint8_t row = 0; // we are considering only first row
    unsigned int len = bitbuffer->bits_per_row[row];

    uint8_t msg_len = SL_ASK_MAX_MESSAGE_LEN;
    unsigned int pos, nb_bytes;
    uint8_t rxBits[2] = {0};

    uint16_t crc, crc_recompute;
    uint8_t data_len, house_id, sensor_type, sensor_count, alarms;
    uint16_t module_id, sensor_value, battery_voltage;

    // Looking for preamble
    uint8_t init_pattern[] = {
      0x55, // 8
      0x55, // 16
      0x55, // 24
      0x51, // 32
      0xcd, // 40
    };
    // The first 0 is ignored by the decoder, so we look only for 28 bits of "01"
    // and not 32. Also "0x1CD" is 0xb38 (SL_ASK_START_SYMBOL) with LSBit first.
    uint8_t init_pattern_len = 40;

    pos = bitbuffer_search(bitbuffer, row, 0, init_pattern, init_pattern_len);
    if(pos == len){
        if(debug_output) {
            printf("SL ASK preamble not found\n");
        }
        return 0;
    }

    // read "bytes" of 12 bit
    nb_bytes=0;
    pos += init_pattern_len;
    for(; pos < len && nb_bytes < msg_len; pos += 12){
        bitbuffer_extract_bytes(bitbuffer, row, pos, rxBits, /*len=*/16);
        // ^ we should read 16 bits and not 12, elsewhere last 4bits are ignored
        rxBits[0] = reverse8(rxBits[0]);
        rxBits[1] = reverse8(rxBits[1]);
        rxBits[1] = ((rxBits[1] & 0x0F)<<2) + (rxBits[0]>>6);
        rxBits[0] &= 0x3F;
        uint8_t hi_nibble = symbol_6to4(rxBits[0]);
        if(hi_nibble > 0xF){
            if(debug_output){
                fprintf(stdout, "Error on 6to4 decoding high nibble: %X\n", rxBits[0]);
            }
            return 0;
        }
        uint8_t lo_nibble = symbol_6to4(rxBits[1]);
        if(lo_nibble > 0xF){
            if(debug_output){
                fprintf(stdout, "Error on 6to4 decoding low nibble: %X\n", rxBits[1]);
            }
            return 0;
        }
        uint8_t byte =  hi_nibble<<4 | lo_nibble;
        sl_payload[nb_bytes] = byte;
        if(nb_bytes == 0){
            msg_len = byte;
        }
        nb_bytes++;
    }

    // Get header
    data_len = msg_len - SL_ASK_HEADER_LEN - 3;
    house_id = sl_payload[1];
    module_id = sl_payload[2] * 256 + sl_payload[3];
    sensor_type = sl_payload[4];
    sensor_count = sl_payload[5];
    alarms = sl_payload[6];
    sensor_value = sl_payload[7] * 256 + sl_payload[8];
    battery_voltage = sl_payload[9] * 256 + sl_payload[10];

    // Check CRC
    crc = sl_payload[5 + data_len] + (sl_payload[5 + data_len + 1]<<8);
    crc_recompute = ~crc16(sl_payload, msg_len-2, 0x8408, 0xFFFF);
    if(crc_recompute != crc){
        if(debug_output){
            fprintf(stdout, "CRC error: %04X != %04X\n", crc_recompute, crc);
        }
        return 0;
    }

    data = data_make(
        "time",             "",                 DATA_STRING,  time_str,
        "model",            "",                 DATA_STRING,  "Sensible Living Mini-Plant Moisture Sensor",
        "house_id",         "House ID",         DATA_INT,     house_id,
        "module_id",        "Module ID",        DATA_INT,     module_id,
        "sensor_type",      "Sensor Type",      DATA_INT,     sensor_type,
        "sensor_count",     "Sensor Count",     DATA_INT,     sensor_count,
        "alarms",           "Alarms",           DATA_INT,     alarms,
        "sensor_value",     "Sensor Value",     DATA_INT,     sensor_value,
        "battery_voltage",  "Battery Voltage",  DATA_INT,     battery_voltage,
        NULL
	);
    data_acquired_handler(data);

    return 0;
}

static char *radiohead_ask_output_fields[] = {
    "time",
    "model",
    "len",
    "to",
    "from",
    "id",
    "flags",
    "payload",
    "mic",
    NULL
};

static char *sensible_living_output_fields[] = {
    "time",
    "model",
    "house_id",
    "module_id",
    "sensor_type",
    "sensor_count",
    "alarms",
    "sensor_value",
    "battery_voltage",
    NULL
};

r_device radiohead_ask = {
    .name           = "Radiohead ASK",
    .modulation     = OOK_PULSE_PCM_RZ,
    .short_limit    = 500,
    .long_limit     = 500,
    .reset_limit    = 5000,
    .json_callback  = &radiohead_ask_callback,
    .fields         = radiohead_ask_output_fields,
};

r_device sensible_living = {
    .name           = "Sensible Living Mini-Plant Moisture Sensor",
    .modulation     = OOK_PULSE_PCM_RZ,
    .short_limit    = SL_ASK_BIT_LEN,
    .long_limit     = SL_ASK_BIT_LEN,
    .reset_limit    = SL_ASK_BIT_LEN*10,
    .json_callback  = &sensible_living_callback,
    .fields         = sensible_living_output_fields,
};
