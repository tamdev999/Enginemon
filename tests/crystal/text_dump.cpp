#include <iostream>
#include <iomanip>
#include "crystal/rom/loader.hpp"
using namespace crystal;

int main(int argc, char* argv[]) {
    if (argc < 2) return 1;
    auto rom = RomData::load(argv[1]);
    if (!rom) return 1;
    
    // NewBarkTownSignText at bank 0x6A, addr 0x42E8
    uint32_t text_addr = rom->bank_to_flat(0x6A, 0x42E8);
    
    std::cout << "First 20 bytes of NewBarkTownSignText:\n";
    for (int i = 0; i < 20; i++) {
        uint8_t b = rom->read_byte(text_addr + i);
        std::cout << std::hex << std::setw(2) << std::setfill('0') << (int)b << " ";
        if ((i + 1) % 10 == 0) std::cout << "\n";
    }
    std::cout << "\n";
    return 0;
}
