// weather_damage_sweep.cpp
// Certifies Crystal DoWeatherModifiers vs Enginemon apply_weather_modifier
// over input damage 1..255 for 7 structural weather configurations.
// Usage: weather_damage_sweep <rom_path> <sym_path>
#include "crystal_differential/oracle_runner.hpp"
#include <cstdio>
int main(int argc, char* argv[])
{
    if(argc < 3){
        std::fprintf(stderr,
            "Usage: %s <rom_path> <sym_path>\n",
            argc > 0 ? argv[0] : "weather_damage_sweep");
        return 2;
    }
    return crystal::oracle::run_weather_damage_sweep(argv[1], argv[2]);
}
