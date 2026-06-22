#pragma once
#include <cstdint>
#include <string>
#include <mutex>

// Tek bir I2C bus'a paylasilan erisim (thread-safe)
class I2CBus {
public:
    explicit I2CBus(const std::string& device = "/dev/i2c-1");
    ~I2CBus();

    bool is_open() const { return fd_ >= 0; }

    // Tek byte yaz: reg = value
    bool write_byte(uint8_t addr, uint8_t reg, uint8_t value);

    // Tek byte oku: reg
    bool read_byte(uint8_t addr, uint8_t reg, uint8_t& value);

    // N byte oku: reg'den baslayarak buf'a
    bool read_bytes(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len);

private:
    int fd_ = -1;
    std::mutex mtx_;  // ayni bus'a paralel erisimi serilestir
    bool select_addr(uint8_t addr);
};