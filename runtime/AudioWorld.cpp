#include "runtime/AudioWorld.hpp"

#include "core/Log.hpp"
#include "scene/Scene.hpp"

#include <glm/geometric.hpp>
#include <glm/matrix.hpp>

#define MA_NO_ENCODING
#define MA_NO_GENERATION
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <unordered_map>
#include <vector>

namespace lumora {

namespace {
/// Clips larger than this stream from disk instead of being decoded up front.
constexpr std::uintmax_t kStreamAboveBytes = 2u * 1024u * 1024u;
} // namespace

struct AudioWorld::Impl {
    Scene& scene;
    std::filesystem::path root;
    bool device = true;
    ma_engine engine{};
    bool engineReady = false;
    struct Voice {
        std::unique_ptr<ma_sound> sound;
        std::string clip;
        bool started = false;
        AudioBus bus = AudioBus::Sfx;
    };
    std::unordered_map<entt::entity, Voice> voices;
    std::unordered_map<std::string, bool> reported;  // clips that failed to load (logged once)
    // Mixer: Master, then one group per AudioBus below it.
    static constexpr int kBuses = 5;
    ma_sound_group master{};
    ma_sound_group buses[kBuses]{};
    bool groupsReady = false;
    float busVolumes[kBuses + 1] = {1, 1, 1, 1, 1, 1};  // index 0 Master, then AudioBus + 1
    bool busMutedFlags[kBuses + 1] = {};

    bool initGroups() {
        if (ma_sound_group_init(&engine, 0, nullptr, &master) != MA_SUCCESS) return false;
        for (int i = 0; i < kBuses; ++i) {
            if (ma_sound_group_init(&engine, 0, &master, &buses[i]) != MA_SUCCESS) {
                for (int j = 0; j < i; ++j) ma_sound_group_uninit(&buses[j]);
                ma_sound_group_uninit(&master);
                return false;
            }
        }
        groupsReady = true;
        return true;
    }
    ma_sound_group* group(AudioBus bus) {
        const int i = static_cast<int>(bus);
        return groupsReady && i >= 0 && i < kBuses ? &buses[i] : nullptr;
    }
    void applyBus(int index) {
        if (!groupsReady) return;
        ma_sound_group* g = index == 0 ? &master : &buses[index - 1];
        ma_sound_group_set_volume(g, busMutedFlags[index] ? 0.0f : busVolumes[index]);
    }

    explicit Impl(Scene& s) : scene(s) {}
    ~Impl() {
        for (auto& [entity, voice] : voices) ma_sound_uninit(voice.sound.get());
        voices.clear();
        if (groupsReady) {
            for (auto& g : buses) ma_sound_group_uninit(&g);
            ma_sound_group_uninit(&master);
        }
        if (engineReady) ma_engine_uninit(&engine);
    }

    void apply(Voice& voice, const AudioSourceComponent& source) {
        ma_sound* s = voice.sound.get();
        ma_sound_set_volume(s, std::max(source.volume, 0.0f));
        ma_sound_set_pitch(s, std::max(source.pitch, 0.01f));
        ma_sound_set_looping(s, source.loop ? MA_TRUE : MA_FALSE);
        ma_sound_set_spatialization_enabled(s, source.spatial ? MA_TRUE : MA_FALSE);
        ma_sound_set_attenuation_model(s, ma_attenuation_model_inverse);
        ma_sound_set_min_distance(s, std::max(source.minDistance, 0.01f));
        ma_sound_set_max_distance(s, std::max(source.maxDistance, source.minDistance + 0.01f));
    }

    bool createVoice(entt::entity e, const AudioSourceComponent& source) {
        if (source.clip.empty()) return false;
        const std::filesystem::path relative = std::filesystem::path(source.clip).lexically_normal();
        if (relative.is_absolute() || relative.empty() || *relative.begin() == "..") {
            if (!reported[source.clip]) log::warn("Audio", "clip '{}' must be inside the project", source.clip);
            reported[source.clip] = true;
            return false;
        }
        const std::filesystem::path file = root / relative;
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        ma_uint32 flags = ec || size > kStreamAboveBytes ? MA_SOUND_FLAG_STREAM : MA_SOUND_FLAG_DECODE;
        auto sound = std::make_unique<ma_sound>();
        const ma_result result = ma_sound_init_from_file(&engine, file.string().c_str(), flags, group(source.bus), nullptr, sound.get());
        if (result != MA_SUCCESS) {
            if (!reported[source.clip]) {
                log::warn("Audio", "cannot play '{}': {}", source.clip, ma_result_description(result));
            }
            reported[source.clip] = true;
            return false;
        }
        Voice voice{std::move(sound), source.clip, false, source.bus};
        apply(voice, source);
        voices.emplace(e, std::move(voice));
        return true;
    }
};

AudioWorld::AudioWorld(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
AudioWorld::~AudioWorld() = default;

bool AudioWorld::usesDevice() const noexcept { return impl_->device; }

Result<std::unique_ptr<AudioWorld>> AudioWorld::create(Scene& scene, const Options& options) {
    auto impl = std::make_unique<Impl>(scene);
    impl->root = options.projectRoot;
    impl->device = options.device;
    ma_engine_config config = ma_engine_config_init();
    if (!options.device) {
        config.noDevice = MA_TRUE;
        config.channels = 2;
        config.sampleRate = 48000;
    }
    const ma_result result = ma_engine_init(&config, &impl->engine);
    if (result != MA_SUCCESS) return makeError("audio unavailable: {}", ma_result_description(result));
    impl->engineReady = true;
    if (!impl->initGroups()) log::warn("Audio", "mixer buses unavailable; sounds play straight to the output");
    log::info("Audio", "audio engine started ({} Hz{})", ma_engine_get_sample_rate(&impl->engine),
              options.device ? "" : ", no device");
    return std::unique_ptr<AudioWorld>(new AudioWorld(std::move(impl)));
}

namespace {
int busIndex(std::string_view name) {
    std::string lower(name);
    for (char& c : lower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower == "master") return 0;
    if (lower == "music") return 1;
    if (lower == "sfx") return 2;
    if (lower == "ui") return 3;
    if (lower == "voice") return 4;
    if (lower == "ambience") return 5;
    return -1;
}
} // namespace

bool AudioWorld::setBusVolume(std::string_view bus, float volume) {
    const int i = busIndex(bus);
    if (i < 0) return false;
    impl_->busVolumes[i] = std::clamp(volume, 0.0f, 4.0f);
    impl_->applyBus(i);
    return true;
}

float AudioWorld::busVolume(std::string_view bus) const {
    const int i = busIndex(bus);
    return i < 0 ? -1.0f : impl_->busVolumes[i];
}

bool AudioWorld::setBusMuted(std::string_view bus, bool muted) {
    const int i = busIndex(bus);
    if (i < 0) return false;
    impl_->busMutedFlags[i] = muted;
    impl_->applyBus(i);
    return true;
}

bool AudioWorld::busMuted(std::string_view bus) const {
    const int i = busIndex(bus);
    return i >= 0 && impl_->busMutedFlags[i];
}

bool AudioWorld::sceneHasAudio(const Scene& scene) {
    return !scene.registry().view<AudioSourceComponent>().empty();
}

void AudioWorld::update() {
    Impl& w = *impl_;
    auto& reg = w.scene.registry();
    // Voices of removed entities (or removed sources) stop.
    for (auto it = w.voices.begin(); it != w.voices.end();) {
        if (!reg.valid(it->first) || !reg.all_of<AudioSourceComponent>(it->first)) {
            ma_sound_uninit(it->second.sound.get());
            it = w.voices.erase(it);
        } else {
            ++it;
        }
    }
    for (auto [e, source] : reg.view<AudioSourceComponent>().each()) {
        auto it = w.voices.find(e);
        if (it == w.voices.end()) {
            if (!w.createVoice(e, source)) continue;
            it = w.voices.find(e);
        } else if (it->second.clip != source.clip || it->second.bus != source.bus) {  // start over with the new clip / bus
            ma_sound_uninit(it->second.sound.get());
            w.voices.erase(it);
            if (!w.createVoice(e, source)) continue;
            it = w.voices.find(e);
        }
        Impl::Voice& voice = it->second;
        w.apply(voice, source);
        if (source.spatial) {
            const glm::vec3 p(w.scene.worldMatrix(e)[3]);
            ma_sound_set_position(voice.sound.get(), p.x, p.y, p.z);
        }
        if (!voice.started) {
            voice.started = true;
            if (source.playOnStart) ma_sound_start(voice.sound.get());
        }
    }
}

void AudioWorld::setListener(const glm::vec3& position, const glm::vec3& forward, const glm::vec3& up) {
    ma_engine_listener_set_position(&impl_->engine, 0, position.x, position.y, position.z);
    ma_engine_listener_set_direction(&impl_->engine, 0, forward.x, forward.y, forward.z);
    ma_engine_listener_set_world_up(&impl_->engine, 0, up.x, up.y, up.z);
}

void AudioWorld::setListenerFromView(const glm::mat4& view) {
    const glm::mat4 camera = glm::inverse(view);
    setListener(glm::vec3(camera[3]), -glm::vec3(camera[2]), glm::vec3(camera[1]));
}

void AudioWorld::setMasterVolume(float volume) {
    ma_engine_set_volume(&impl_->engine, std::clamp(volume, 0.0f, 1.0f));
}

void AudioWorld::setPaused(bool paused) {
    if (!impl_->device) return;
    if (paused) {
        ma_engine_stop(&impl_->engine);
    } else {
        ma_engine_start(&impl_->engine);
    }
}

bool AudioWorld::play(entt::entity entity) {
    update();  // a source added this step gets its voice now
    const auto it = impl_->voices.find(entity);
    if (it == impl_->voices.end()) return false;
    it->second.started = true;
    ma_sound_seek_to_pcm_frame(it->second.sound.get(), 0);
    return ma_sound_start(it->second.sound.get()) == MA_SUCCESS;
}

bool AudioWorld::stop(entt::entity entity) {
    const auto it = impl_->voices.find(entity);
    if (it == impl_->voices.end()) return false;
    return ma_sound_stop(it->second.sound.get()) == MA_SUCCESS;
}

bool AudioWorld::isPlaying(entt::entity entity) const {
    const auto it = impl_->voices.find(entity);
    if (it == impl_->voices.end()) return false;
    ma_sound* sound = it->second.sound.get();
    // A finished non-looping sound reports "at end" before its playing flag settles.
    return ma_sound_is_playing(sound) == MA_TRUE && ma_sound_at_end(sound) == MA_FALSE;
}

float AudioWorld::cursorSeconds(entt::entity entity) const {
    const auto it = impl_->voices.find(entity);
    if (it == impl_->voices.end()) return 0.0f;
    float seconds = 0.0f;
    ma_sound_get_cursor_in_seconds(it->second.sound.get(), &seconds);
    return seconds;
}

std::size_t AudioWorld::voiceCount() const { return impl_->voices.size(); }

float AudioWorld::mix(std::uint32_t frames) {
    std::vector<float> buffer(static_cast<std::size_t>(frames) * 2);
    ma_uint64 read = 0;
    ma_engine_read_pcm_frames(&impl_->engine, buffer.data(), frames, &read);
    float peak = 0.0f;
    for (std::size_t i = 0; i < static_cast<std::size_t>(read) * 2; ++i) peak = std::max(peak, std::abs(buffer[i]));
    return peak;
}

} // namespace lumora
