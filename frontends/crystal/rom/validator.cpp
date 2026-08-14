// crystal/rom/validator.cpp
// ROM validation - implementation in loader.cpp
// This file exists for future extended validation logic

#include "crystal/rom/loader.hpp"
#include "crystal/rom/profile.hpp"

namespace crystal {

// Extended validation could go here:
// - Verify specific ROM regions match expected patterns
// - Check for known ROM patches/hacks
// - Validate internal checksums
// - Detect region/language variants

// For now, validation is handled by validate_crystal_rom() in loader.cpp

} // namespace crystal
