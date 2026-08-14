// crystal/rom/symbol_map.cpp
// RGBDS symbol file parsing

#include "crystal/rom/symbol_map.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <regex>

namespace crystal {

Symbol SymbolMap::parse_line(const std::string& line) {
    Symbol sym;
    sym.type = SymbolType::Unknown;
    sym.bank = 0;
    sym.address = 0;
    sym.flat_address = 0;
    sym.line = 0;
    
    // RGBDS .sym format: BB:AAAA NAME
    // BB = bank (hex), AAAA = address (hex)
    std::regex sym_regex(R"(([0-9A-Fa-f]+):([0-9A-Fa-f]+)\s+(.+))");
    std::smatch match;
    
    if (std::regex_match(line, match, sym_regex)) {
        sym.bank = std::stoul(match[1].str(), nullptr, 16);
        sym.address = std::stoul(match[2].str(), nullptr, 16);
        sym.name = match[3].str();
        sym.type = SymbolType::Label;
        
        // Calculate flat address
        if (sym.address < 0x4000) {
            sym.flat_address = sym.address;
        } else {
            sym.flat_address = (sym.bank * 0x4000) + (sym.address - 0x4000);
        }
    }
    
    return sym;
}

void SymbolMap::build_indexes() {
    by_name_.clear();
    by_address_.clear();
    
    for (size_t i = 0; i < symbols_.size(); ++i) {
        by_name_[symbols_[i].name] = i;
        by_address_[symbols_[i].flat_address] = i;
    }
}

std::unique_ptr<SymbolMap> SymbolMap::load(const std::filesystem::path& path) {
    std::ifstream file(path);
    if (!file) {
        return nullptr;
    }
    
    auto map = std::make_unique<SymbolMap>();
    std::string line;
    
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == ';') continue;
        
        Symbol sym = parse_line(line);
        if (sym.type != SymbolType::Unknown) {
            map->symbols_.push_back(std::move(sym));
        }
    }
    
    map->build_indexes();
    return map;
}

std::unique_ptr<SymbolMap> SymbolMap::load_multiple(
    const std::vector<std::filesystem::path>& paths) {
    auto map = std::make_unique<SymbolMap>();
    
    for (const auto& path : paths) {
        std::ifstream file(path);
        if (!file) continue;
        
        std::string line;
        while (std::getline(file, line)) {
            if (line.empty() || line[0] == ';') continue;
            
            Symbol sym = parse_line(line);
            if (sym.type != SymbolType::Unknown) {
                sym.file = path.filename().string();
                map->symbols_.push_back(std::move(sym));
            }
        }
    }
    
    map->build_indexes();
    return map;
}

const Symbol* SymbolMap::find(const std::string& name) const {
    auto it = by_name_.find(name);
    if (it != by_name_.end()) {
        return &symbols_[it->second];
    }
    return nullptr;
}

const Symbol* SymbolMap::find_at(uint32_t flat_address) const {
    // Find exact match first
    auto it = by_address_.find(flat_address);
    if (it != by_address_.end()) {
        return &symbols_[it->second];
    }
    
    // Find closest symbol before this address
    const Symbol* closest = nullptr;
    uint32_t closest_addr = 0;
    
    for (const auto& sym : symbols_) {
        if (sym.flat_address <= flat_address && 
            sym.flat_address > closest_addr) {
            closest = &sym;
            closest_addr = sym.flat_address;
        }
    }
    
    return closest;
}

std::vector<const Symbol*> SymbolMap::find_prefix(const std::string& prefix) const {
    std::vector<const Symbol*> result;
    
    for (const auto& sym : symbols_) {
        if (sym.name.compare(0, prefix.length(), prefix) == 0) {
            result.push_back(&sym);
        }
    }
    
    return result;
}

std::vector<const Symbol*> SymbolMap::find_in_bank(uint8_t bank) const {
    std::vector<const Symbol*> result;
    
    for (const auto& sym : symbols_) {
        if (sym.bank == bank) {
            result.push_back(&sym);
        }
    }
    
    return result;
}

bool SymbolMap::has(const std::string& name) const {
    return by_name_.find(name) != by_name_.end();
}

std::optional<uint32_t> SymbolMap::address(const std::string& name) const {
    auto* sym = find(name);
    if (sym) {
        return sym->flat_address;
    }
    return std::nullopt;
}

std::optional<std::string> SymbolMap::name_at(uint32_t flat_address) const {
    auto it = by_address_.find(flat_address);
    if (it != by_address_.end()) {
        return symbols_[it->second].name;
    }
    return std::nullopt;
}

} // namespace crystal
