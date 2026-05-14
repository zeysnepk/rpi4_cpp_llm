#include "i2c_bus.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/i2c-dev.h>
#include <iostream>

I2CBus::I2CBus(const std::string& device) {
    fd_ = open(device.c_str(), O_RDWR);
    if (fd_ < 0) {
        std::cerr << "I2C: " << device << " acilamadi\n";
    }
}

I2CBus::~I2CBus() {
    if (fd_ >= 0) close(fd_);
}

bool I2CBus::select_addr(uint8_t addr) {
    return ioctl(fd_, I2C_SLAVE, addr) >= 0;
}

bool I2CBus::write_byte(uint8_t addr, uint8_t reg, uint8_t value) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!select_addr(addr)) return false;
    uint8_t buf[2] = { reg, value };
    return ::write(fd_, buf, 2) == 2;
}

bool I2CBus::read_byte(uint8_t addr, uint8_t reg, uint8_t& value) {
    return read_bytes(addr, reg, &value, 1);
}

bool I2CBus::read_bytes(uint8_t addr, uint8_t reg, uint8_t* buf, size_t len) {
    std::lock_guard<std::mutex> lk(mtx_);
    if (!select_addr(addr)) return false;
    if (::write(fd_, &reg, 1) != 1) return false;
    return ::read(fd_, buf, len) == (ssize_t)len;
}