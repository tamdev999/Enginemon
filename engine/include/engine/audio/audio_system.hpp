#pragma once
// engine/audio/audio_system.hpp
// Native audio system with volume buses
// AudioSequence representation, synthesis, and SDL3 output

#include "engine/core/types.hpp"
#include <memory>
#include <string>
#include <vector>
#include <unordered_map>
#include <optional>
#include <functional>

namespace enginemon {

// Forward declarations
class AudioMixer;
class AudioSynth;
class AudioDevice;

// Volume bus types
enum class AudioBus : uint8_t {
    Master,
    OverworldMusic,
    BattleMusic,
    EventMusic,     // Cutscene/menu music
    SFX,
    UI,
    Ambience,
    
    COUNT
};

// Audio channel (Game Boy had 4, we can have more)
enum class AudioChannel : uint8_t {
    Pulse1,         // Square wave 1 (with sweep)
    Pulse2,         // Square wave 2
    Wave,           // Custom waveform
    Noise,          // Noise generator
    
    // Extended channels for mods
    Sample,         // PCM sample playback
    
    COUNT
};

// ============================================================================
// AudioSequence - translated Crystal music/SFX format
// ============================================================================

// Note command
struct NoteCmd {
    uint8_t pitch;          // MIDI-style pitch (0-127)
    uint8_t velocity;       // Volume (0-127)
    uint16_t duration;      // In ticks
};

// Rest command
struct RestCmd {
    uint16_t duration;      // In ticks
};

// Tempo change
struct TempoCmd {
    uint16_t bpm;
};

// Duty cycle change (for pulse channels)
struct DutyCmd {
    uint8_t duty;           // 0=12.5%, 1=25%, 2=50%, 3=75%
};

// Volume envelope
struct EnvelopeCmd {
    uint8_t initial;        // Initial volume
    int8_t direction;       // +1 or -1
    uint8_t pace;           // Speed of change
};

// Pitch slide
struct SlideCmd {
    int8_t delta;           // Pitch change per tick
    uint8_t duration;       // How long to slide
};

// Vibrato
struct VibratoCmd {
    uint8_t depth;          // Pitch variation
    uint8_t rate;           // Speed of vibrato
};

// Pan (stereo position)
struct PanCmd {
    uint8_t left;           // 0-15
    uint8_t right;          // 0-15
};

// Loop start
struct LoopStartCmd {
    uint8_t count;          // Number of iterations (0 = infinite)
};

// Loop end (jump back to loop start)
struct LoopEndCmd {};

// Call subroutine
struct CallCmd {
    uint16_t target;        // Offset in command stream
};

// Return from subroutine
struct ReturnCmd {};

// End of track
struct EndCmd {};

// Drum note (noise channel)
struct DrumCmd {
    uint8_t drum_id;        // Index into drum kit
    uint16_t duration;
};

// Wave pattern (for wave channel)
struct WavePatternCmd {
    std::array<uint8_t, 16> pattern;    // 32 4-bit samples
};

// Sequence command union
using SequenceCommand = std::variant<
    NoteCmd,
    RestCmd,
    TempoCmd,
    DutyCmd,
    EnvelopeCmd,
    SlideCmd,
    VibratoCmd,
    PanCmd,
    LoopStartCmd,
    LoopEndCmd,
    CallCmd,
    ReturnCmd,
    EndCmd,
    DrumCmd,
    WavePatternCmd
>;

// Single channel track
struct AudioTrack {
    AudioChannel channel;
    std::vector<SequenceCommand> commands;
};

// Complete music/SFX sequence
struct AudioSequence {
    MusicId id;             // If music
    SfxId sfx_id;           // If SFX (0 if music)
    std::string name;
    
    uint16_t default_tempo = 120;
    
    std::vector<AudioTrack> tracks;
    
    bool is_sfx() const { return sfx_id != 0; }
    bool is_music() const { return !is_sfx(); }
};

// ============================================================================
// AudioSystem - main interface
// ============================================================================

class AudioSystem {
public:
    AudioSystem();
    ~AudioSystem();
    
    // Initialization
    bool initialize();
    void shutdown();
    
    // Music control
    void play_music(MusicId id);
    void play_music(const AudioSequence& seq);
    void stop_music();
    void pause_music();
    void resume_music();
    void fade_out_music(float seconds);
    void cross_fade_music(MusicId new_id, float seconds);
    
    // SFX control
    void play_sfx(SfxId id);
    void play_sfx(const AudioSequence& seq);
    void stop_sfx(SfxId id);
    void stop_all_sfx();
    bool is_sfx_playing(SfxId id) const;
    void wait_sfx_complete(SfxId id, std::function<void()> callback);
    
    // Pokemon cries
    void play_cry(SpeciesId species);
    void play_cry_pitched(SpeciesId species, float pitch_mult);
    
    // Volume buses
    void set_bus_volume(AudioBus bus, float volume);  // 0.0 - 1.0
    float get_bus_volume(AudioBus bus) const;
    void mute_bus(AudioBus bus, bool muted);
    bool is_bus_muted(AudioBus bus) const;
    
    // Master volume
    void set_master_volume(float volume);
    float get_master_volume() const;
    
    // Asset loading
    void load_sequence(const AudioSequence& seq);
    void load_music_file(const std::filesystem::path& path);
    void load_sfx_file(const std::filesystem::path& path);
    
    // Update (call each frame)
    void update(float delta_time);
    
    // Current state
    MusicId current_music() const;
    bool is_music_playing() const;
    bool is_any_sfx_playing() const;

private:
    std::unique_ptr<AudioMixer> mixer_;
    std::unique_ptr<AudioSynth> synth_;
    std::unique_ptr<AudioDevice> device_;
    
    std::unordered_map<MusicId, AudioSequence> music_cache_;
    std::unordered_map<SfxId, AudioSequence> sfx_cache_;
    
    std::array<float, static_cast<size_t>(AudioBus::COUNT)> bus_volumes_;
    std::array<bool, static_cast<size_t>(AudioBus::COUNT)> bus_muted_;
    
    float master_volume_ = 1.0f;
    MusicId current_music_id_ = 0;
};

} // namespace enginemon
