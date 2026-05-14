#include "bme280.hpp"
#include <thread>
#include <chrono>
#include <iostream>

BME280::BME280(I2CBus& bus, uint8_t address) : bus_(bus), addr_(address) {}

bool BME280::init() {
    online_ = false;

    // Chip ID kontrolu (0xD0 == 0x60)
    uint8_t chip_id = 0;
    if (!bus_.read_byte(addr_, 0xD0, chip_id) || chip_id != 0x60) {
        std::cerr << "BME280: chip ID yanlis (0x" << std::hex << (int)chip_id << ")\n";
        return false;
    }

    // Soft reset
    bus_.write_byte(addr_, 0xE0, 0xB6);
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    if (!read_calibration()) return false;

    // ctrl_hum: humidity x1 oversampling
    bus_.write_byte(addr_, 0xF2, 0x01);
    // ctrl_meas: temp x1, press x1, normal mode
    bus_.write_byte(addr_, 0xF4, (1 << 5) | (1 << 2) | 0b11);
    // config: 1000ms standby, no IIR
    bus_.write_byte(addr_, 0xF5, 0xA0);

    online_ = true;
    return true;
}

bool BME280::read_calibration() {
    uint8_t buf[26];
    if (!bus_.read_bytes(addr_, 0x88, buf, 26)) return false;

    T1 = (uint16_t)(buf[0]  | (buf[1]  << 8));
    T2 = (int16_t) (buf[2]  | (buf[3]  << 8));
    T3 = (int16_t) (buf[4]  | (buf[5]  << 8));
    P1 = (uint16_t)(buf[6]  | (buf[7]  << 8));
    P2 = (int16_t) (buf[8]  | (buf[9]  << 8));
    P3 = (int16_t) (buf[10] | (buf[11] << 8));
    P4 = (int16_t) (buf[12] | (buf[13] << 8));
    P5 = (int16_t) (buf[14] | (buf[15] << 8));
    P6 = (int16_t) (buf[16] | (buf[17] << 8));
    P7 = (int16_t) (buf[18] | (buf[19] << 8));
    P8 = (int16_t) (buf[20] | (buf[21] << 8));
    P9 = (int16_t) (buf[22] | (buf[23] << 8));
    H1 = buf[25];

    uint8_t h_buf[7];
    if (!bus_.read_bytes(addr_, 0xE1, h_buf, 7)) return false;
    H2 = (int16_t)(h_buf[0] | (h_buf[1] << 8));
    H3 = h_buf[2];
    H4 = (int16_t)((h_buf[3] << 4) | (h_buf[4] & 0x0F));
    H5 = (int16_t)((h_buf[5] << 4) | ((h_buf[4] >> 4) & 0x0F));
    H6 = (int8_t)h_buf[6];

    // 12-bit signed extension (H4, H5)
    if (H4 & 0x800) H4 |= 0xF000;
    if (H5 & 0x800) H5 |= 0xF000;

    return true;
}

nlohmann::json BME280::read() {
    if (!online_) return {};
    uint8_t buf[8];
    if (!bus_.read_bytes(addr_, 0xF7, buf, 8)) return {};

    int32_t adc_P = ((int32_t)buf[0] << 12) | ((int32_t)buf[1] << 4) | (buf[2] >> 4);
    int32_t adc_T = ((int32_t)buf[3] << 12) | ((int32_t)buf[4] << 4) | (buf[5] >> 4);
    int32_t adc_H = ((int32_t)buf[6] << 8)  |  (int32_t)buf[7];

    // Sicaklik kompansasyonu (Bosch datasheet)
    int32_t var1 = ((((adc_T >> 3) - ((int32_t)T1 << 1))) * ((int32_t)T2)) >> 11;
    int32_t var2 = (((((adc_T >> 4) - ((int32_t)T1)) * ((adc_T >> 4) - ((int32_t)T1))) >> 12) *
                   ((int32_t)T3)) >> 14;
    int32_t t_fine = var1 + var2;
    double temperature = ((t_fine * 5 + 128) >> 8) / 100.0;

    // Basinc
    int64_t pv1 = (int64_t)t_fine - 128000;
    int64_t pv2 = pv1 * pv1 * (int64_t)P6;
    pv2 += (pv1 * (int64_t)P5) << 17;
    pv2 += ((int64_t)P4) << 35;
    pv1 = ((pv1 * pv1 * (int64_t)P3) >> 8) + ((pv1 * (int64_t)P2) << 12);
    pv1 = (((((int64_t)1) << 47) + pv1) * (int64_t)P1) >> 33;
    double pressure = 0;
    if (pv1 != 0) {
        int64_t p = 1048576 - adc_P;
        p = (((p << 31) - pv2) * 3125) / pv1;
        pv1 = (((int64_t)P9) * (p >> 13) * (p >> 13)) >> 25;
        pv2 = (((int64_t)P8) * p) >> 19;
        p = ((p + pv1 + pv2) >> 8) + (((int64_t)P7) << 4);
        pressure = (double)p / 256.0 / 100.0;  // hPa
    }

    // Nem
    int32_t v = t_fine - 76800;
    v = (((((adc_H << 14) - (((int32_t)H4) << 20) - (((int32_t)H5) * v)) + ((int32_t)16384)) >> 15) *
         (((((((v * ((int32_t)H6)) >> 10) * (((v * ((int32_t)H3)) >> 11) + ((int32_t)32768))) >> 10) +
           ((int32_t)2097152)) * ((int32_t)H2) + 8192) >> 14));
    v = v - (((((v >> 15) * (v >> 15)) >> 7) * ((int32_t)H1)) >> 4);
    if (v < 0) v = 0;
    if (v > 419430400) v = 419430400;
    double humidity = (v >> 12) / 1024.0;

    return {
        {"temperature_c",  temperature},
        {"pressure_hpa",   pressure},
        {"humidity_pct",   humidity}
    };
}