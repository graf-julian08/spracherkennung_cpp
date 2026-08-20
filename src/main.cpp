#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <mosquitto.h>
#include <portaudio.h>
#include <whisper.h>

constexpr int kSampleRate = 16000;
constexpr int kFramesPerBuffer = 512;
constexpr float kChunkSeconds = 0.2f;   // Hyper-Speed: 0.2s
constexpr float kContextSeconds = 8.0f; // Deep Context: 8.0s
constexpr size_t kChunkSamples =
    static_cast<size_t>(kSampleRate * kChunkSeconds);
constexpr size_t kHistorySamples =
    static_cast<size_t>(kSampleRate * 10.0f); // 10s history

std::atomic<bool> g_running{true};

void HandleSignal(int) { g_running.store(false); }

std::string Trim(const std::string &text) {
  const auto begin = text.find_first_not_of(" \t\n\r");
  if (begin == std::string::npos) {
    return "";
  }
  const auto end = text.find_last_not_of(" \t\n\r");
  return text.substr(begin, end - begin + 1);
}

// --- Levenshtein Distance for Fuzzy Matching ---
int LevenshteinDistance(const std::string &s1, const std::string &s2) {
  const std::size_t m = s1.size();
  const std::size_t n = s2.size();
  std::vector<std::vector<int>> dp(m + 1, std::vector<int>(n + 1));

  for (std::size_t i = 0; i <= m; ++i)
    dp[i][0] = i;
  for (std::size_t j = 0; j <= n; ++j)
    dp[0][j] = j;

  for (std::size_t i = 1; i <= m; ++i) {
    for (std::size_t j = 1; j <= n; ++j) {
      if (std::tolower(s1[i - 1]) == std::tolower(s2[j - 1])) {
        dp[i][j] = dp[i - 1][j - 1];
      } else {
        dp[i][j] = 1 + std::min({dp[i - 1][j], dp[i][j - 1], dp[i - 1][j - 1]});
      }
    }
  }
  return dp[m][n];
}

bool FuzzyMatch(const std::string &text, const std::string &keyword,
                int max_dist = -1) {
  // Dynamic max_dist: Strict for short words
  if (max_dist == -1) {
    if (keyword.size() <= 3)
      max_dist = 0; // Strict for "an", "aus", "rot"
    else
      max_dist = 1;
  }

  // Simple sliding window search
  std::string lower_text = text;
  std::string lower_keyword = keyword;
  std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                 ::tolower);
  std::transform(lower_keyword.begin(), lower_keyword.end(),
                 lower_keyword.begin(), ::tolower);

  std::stringstream ss(lower_text);
  std::string word;
  while (ss >> word) {
    // Remove punctuation from word for better matching
    word.erase(std::remove_if(word.begin(), word.end(), ::ispunct), word.end());

    if (LevenshteinDistance(word, lower_keyword) <= max_dist)
      return true;
  }
  return false;
}

// --- MQTT Controller ---

struct DeviceState {
  std::string state = ""; // "ON", "OFF"
  int r = -1, g = -1, b = -1;
  int brightness = -1;
};

class ZigbeeController {
public:
  ZigbeeController(const char *host, int port) {
    mosquitto_lib_init();
    mosq_ = mosquitto_new("aurora_cpp_native", true, nullptr);
    if (!mosq_) {
      std::cerr << "[MQTT] Failed to create instance.\n";
      return;
    }

    if (mosquitto_connect(mosq_, host, port, 60) != MOSQ_ERR_SUCCESS) {
      std::cerr << "[MQTT] Connect failed.\n";
      return;
    }

    mosquitto_loop_start(mosq_);
    connected_ = true;
    std::cout << "[MQTT] Connected to " << host << ":" << port << "\n";
  }

  ~ZigbeeController() {
    if (mosq_) {
      mosquitto_disconnect(mosq_);
      mosquitto_loop_stop(mosq_, false);
      mosquitto_destroy(mosq_);
    }
    mosquitto_lib_cleanup();
  }

  void SetColor(const std::string &device_id, int r, int g, int b) {
    if (!connected_)
      return;

    // Check Cache
    if (cache_[device_id].state == "ON" && cache_[device_id].r == r &&
        cache_[device_id].g == g && cache_[device_id].b == b) {
      return; // Already in this state
    }

    std::stringstream ss;
    ss << "{\"state\": \"ON\", \"color\": {\"r\": " << r << ", \"g\": " << g
       << ", \"b\": " << b << "}}";
    std::string payload = ss.str();

    std::string topic = "zigbee2mqtt/" + device_id + "/set";
    mosquitto_publish(mosq_, nullptr, topic.c_str(), payload.size(),
                      payload.c_str(), 0, false);
    std::cout << "[ACTION] Set " << device_id << " to RGB(" << r << "," << g
              << "," << b << ")\n";

    // Update Cache
    cache_[device_id].state = "ON";
    cache_[device_id].r = r;
    cache_[device_id].g = g;
    cache_[device_id].b = b;
  }

  void SetState(const std::string &device_id, const std::string &state) {
    if (!connected_)
      return;

    // Check Cache
    if (cache_[device_id].state == state) {
      return; // Already in this state
    }

    std::stringstream ss;
    ss << "{\"state\": \"" << state << "\"}";
    std::string payload = ss.str();

    std::string topic = "zigbee2mqtt/" + device_id + "/set";
    mosquitto_publish(mosq_, nullptr, topic.c_str(), payload.size(),
                      payload.c_str(), 0, false);
    std::cout << "[ACTION] Set " << device_id << " to " << state << "\n";

    // Update Cache
    cache_[device_id].state = state;
  }

  void SetBrightness(const std::string &device_id, int brightness) {
    if (!connected_)
      return;

    // Check Cache (Approximate check to allow minor adjustments if needed, but
    // exact matches skipped)
    if (cache_[device_id].state == "ON" &&
        cache_[device_id].brightness == brightness) {
      return;
    }

    std::stringstream ss;
    ss << "{\"state\": \"ON\", \"brightness\": " << brightness << "}";
    std::string payload = ss.str();

    std::string topic = "zigbee2mqtt/" + device_id + "/set";
    mosquitto_publish(mosq_, nullptr, topic.c_str(), payload.size(),
                      payload.c_str(), 0, false);
    std::cout << "[ACTION] Set " << device_id << " to Brightness " << brightness
              << "\n";

    // Update Cache
    cache_[device_id].state = "ON";
    cache_[device_id].brightness = brightness;
  }

  void StepBrightness(const std::string &device_id, int step) {
    if (!connected_)
      return;

    // Cannot cache relative steps easily without reading back state, so we
    // always execute. But we update cache state to ON.

    std::stringstream ss;
    ss << "{\"state\": \"ON\", \"brightness_step\": " << step << "}";
    std::string payload = ss.str();

    std::string topic = "zigbee2mqtt/" + device_id + "/set";
    mosquitto_publish(mosq_, nullptr, topic.c_str(), payload.size(),
                      payload.c_str(), 0, false);
    std::cout << "[ACTION] Step " << device_id << " Brightness by " << step
              << "\n";

    cache_[device_id].state = "ON";
    // Invalidate brightness cache since we don't know the new absolute value
    cache_[device_id].brightness = -1;
  }

private:
  struct mosquitto *mosq_ = nullptr;
  bool connected_ = false;
  std::map<std::string, DeviceState> cache_;
};

// --- Advanced Command Parser ---

// Global Intent Tracker for Deduplication
struct IntentTracker {
  std::string last_action = "";
  std::chrono::steady_clock::time_point last_time;
};

static IntentTracker g_intent_tracker;

// --- Simple & Fast Command Parser ---

bool ExecuteCommand(const std::string &text, ZigbeeController &zigbee) {
  // Lamps
  std::vector<std::string> lamps = {
      "0xa4c1380b3907bce7", // Fenster
      "0xa4c138573c6833f2", // Pult
      "0xa4c1388150c14677"  // Bett
  };

  // 1. Triggers (Fast Fuzzy Match)
  bool has_lamp = FuzzyMatch(text, "Lampe") || FuzzyMatch(text, "Licht", 0) ||
                  FuzzyMatch(text, "Liecht") || FuzzyMatch(text, "Lampere") ||
                  FuzzyMatch(text, "Lamphe") || FuzzyMatch(text, "Lhampe") ||
                  FuzzyMatch(text, "Landpe") || FuzzyMatch(text, "Landperf") ||
                  FuzzyMatch(text, "Landpör");

  bool has_brightness_trigger =
      FuzzyMatch(text, "Helligkeit") || FuzzyMatch(text, "Helikreite") ||
      FuzzyMatch(text, "Helligkeiten") || FuzzyMatch(text, "Helligkeite");

  if (!has_lamp && !has_brightness_trigger) {
    return false;
  }

  std::string current_action = "";
  bool action_taken = false;

  // 1. State & Color
  if (has_lamp) {
    // CRITICAL FIX: "Auf" must NEVER trigger "Aus".
    // If the text contains "auf" (context: "auf Gelb"), ignore "aus" triggers.
    bool contains_auf = FuzzyMatch(text, "auf", 0);

    // State: OFF
    if (!contains_auf && !FuzzyMatch(text, "Farbe") &&
        (FuzzyMatch(text, "aus", 0) || FuzzyMatch(text, "ausschalten"))) {
      current_action = "STATE_OFF";
      // Deduplication
      auto now = std::chrono::steady_clock::now();
      if (g_intent_tracker.last_action == current_action &&
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - g_intent_tracker.last_time)
                  .count() < 2000) {
        return false;
      }
      g_intent_tracker.last_action = current_action;
      g_intent_tracker.last_time = now;

      for (const auto &lamp : lamps) {
        zigbee.SetState(lamp, "OFF");
        std::this_thread::sleep_for(std::chrono::milliseconds(20)); // Fast
      }
      action_taken = true;
    }
    // State: ON
    else if (FuzzyMatch(text, "an", 0) || FuzzyMatch(text, "ein", 0) ||
             FuzzyMatch(text, "einschalten") || FuzzyMatch(text, "anmachen")) {
      current_action = "STATE_ON";
      auto now = std::chrono::steady_clock::now();
      if (g_intent_tracker.last_action == current_action &&
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - g_intent_tracker.last_time)
                  .count() < 2000) {
        return false;
      }
      g_intent_tracker.last_action = current_action;
      g_intent_tracker.last_time = now;

      for (const auto &lamp : lamps) {
        zigbee.SetState(lamp, "ON");
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      action_taken = true;
    }

    // Colors (Expanded Palette)
    int r = 0, g = 0, b = 0;
    bool color_found = false;
    std::string color_name = "";

    if (FuzzyMatch(text, "Rot") || FuzzyMatch(text, "Rote") ||
        FuzzyMatch(text, "Red") || FuzzyMatch(text, "Verrote") ||
        FuzzyMatch(text, "Rout") || FuzzyMatch(text, "Route") ||
        FuzzyMatch(text, "Road") || FuzzyMatch(text, "Roth") ||
        FuzzyMatch(text, "Ruhd")) {
      r = 255;
      g = 0;
      b = 0;
      color_found = true;
      color_name = "RED";
    } else if (FuzzyMatch(text, "Grün") || FuzzyMatch(text, "Green")) {
      r = 0;
      g = 255;
      b = 0;
      color_found = true;
      color_name = "GREEN";
    } else if (FuzzyMatch(text, "Blau") || FuzzyMatch(text, "Blue")) {
      r = 0;
      g = 0;
      b = 255;
      color_found = true;
      color_name = "BLUE";
    } else if (FuzzyMatch(text, "Gelb") || FuzzyMatch(text, "Yellow") ||
               FuzzyMatch(text, "Geld")) {
      r = 255;
      g = 255;
      b = 0;
      color_found = true;
      color_name = "YELLOW";
    } else if (FuzzyMatch(text, "Orange")) {
      r = 255;
      g = 165;
      b = 0;
      color_found = true;
      color_name = "ORANGE";
    } else if (FuzzyMatch(text, "Violett") || FuzzyMatch(text, "Lila") ||
               FuzzyMatch(text, "Purple")) {
      r = 238;
      g = 130;
      b = 238;
      color_found = true;
      color_name = "PURPLE";
    } else if (FuzzyMatch(text, "Pink") || FuzzyMatch(text, "Rosa") ||
               FuzzyMatch(text, "Pingre")) {
      r = 255;
      g = 105;
      b = 180;
      color_found = true;
      color_name = "PINK";
    } else if (FuzzyMatch(text, "Magenta")) {
      r = 255;
      g = 0;
      b = 255;
      color_found = true;
      color_name = "MAGENTA";
    } else if (FuzzyMatch(text, "Cyan") || FuzzyMatch(text, "Türkis")) {
      r = 0;
      g = 255;
      b = 255;
      color_found = true;
      color_name = "CYAN";
    } else if (FuzzyMatch(text, "Lime") || FuzzyMatch(text, "Limette")) {
      r = 50;
      g = 205;
      b = 50;
      color_found = true;
      color_name = "LIME";
    } else if (FuzzyMatch(text, "Gold")) {
      r = 255;
      g = 215;
      b = 0;
      color_found = true;
      color_name = "GOLD";
    } else if (FuzzyMatch(text, "Weiß") || FuzzyMatch(text, "White")) {
      r = 255;
      g = 255;
      b = 255;
      color_found = true;
      color_name = "WHITE";
    } else if (FuzzyMatch(text, "Warm")) {
      r = 255;
      g = 214;
      b = 170;
      color_found = true;
      color_name = "WARM";
    } else if (FuzzyMatch(text, "Kalt") || FuzzyMatch(text, "Cold")) {
      r = 255;
      g = 255;
      b = 255;
      color_found = true;
      color_name = "COLD";
    }

    if (color_found) {
      current_action = "COLOR_" + color_name;
      auto now = std::chrono::steady_clock::now();
      if (g_intent_tracker.last_action == current_action &&
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - g_intent_tracker.last_time)
                  .count() < 2000) {
        return false;
      }
      g_intent_tracker.last_action = current_action;
      g_intent_tracker.last_time = now;

      for (const auto &lamp : lamps) {
        zigbee.SetColor(lamp, r, g, b);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      action_taken = true;
    }
  }

  // 2. Brightness (Relative)
  int step = 0;
  if (FuzzyMatch(text, "heller") || FuzzyMatch(text, "Heller")) {
    step = 40;
  } else if (FuzzyMatch(text, "dunkler") || FuzzyMatch(text, "Dunkler")) {
    step = -40;
  }

  if (step != 0) {
    current_action = "BRIGHTNESS_STEP_" + std::to_string(step);

    // Anti-Oscillation & Deduplication for Relative Brightness
    auto now = std::chrono::steady_clock::now();

    // 1. Deduplication (Lock time 2.5s for steps to prevent rapid fire)
    if (g_intent_tracker.last_action == current_action &&
        std::chrono::duration_cast<std::chrono::milliseconds>(
            now - g_intent_tracker.last_time)
                .count() < 2500) {
      return false;
    }

    // 2. Anti-Oscillation (Prevent Heller <-> Dunkler flip-flops)
    bool is_opposing =
        (g_intent_tracker.last_action == "BRIGHTNESS_STEP_40" && step == -40) ||
        (g_intent_tracker.last_action == "BRIGHTNESS_STEP_-40" && step == 40);

    if (is_opposing && std::chrono::duration_cast<std::chrono::milliseconds>(
                           now - g_intent_tracker.last_time)
                               .count() < 3000) {
      // Ignore opposing command if too soon
      return false;
    }

    g_intent_tracker.last_action = current_action;
    g_intent_tracker.last_time = now;

    for (const auto &lamp : lamps) {
      zigbee.StepBrightness(lamp, step);
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    action_taken = true;
  }

  // 3. Brightness (Absolute)
  std::string lower_text = text;
  std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(),
                 ::tolower);

  if (lower_text.find("prozent") != std::string::npos ||
      lower_text.find("%") != std::string::npos) {
    std::stringstream ss(lower_text);
    std::string word;
    int val = -1;
    while (ss >> word) {
      if (std::isdigit(word[0])) {
        try {
          val = std::stoi(word);
        } catch (...) {
        }
      }
    }

    if (val >= 0 && val <= 100) {
      current_action = "BRIGHTNESS_ABS_" + std::to_string(val);
      auto now = std::chrono::steady_clock::now();
      if (g_intent_tracker.last_action == current_action &&
          std::chrono::duration_cast<std::chrono::milliseconds>(
              now - g_intent_tracker.last_time)
                  .count() < 2000) {
        return false;
      }
      g_intent_tracker.last_action = current_action;
      g_intent_tracker.last_time = now;

      int brightness = static_cast<int>((val / 100.0f) * 254.0f);
      for (const auto &lamp : lamps) {
        zigbee.SetBrightness(lamp, brightness);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
      action_taken = true;
    }
  }

  return action_taken;
}

// --- Audio & VAD ---

class VoiceActivityDetector {
public:
  explicit VoiceActivityDetector(int sample_rate)
      : frame_samples_(sample_rate / 100), // 10 ms
        hangover_frames_(10), history_limit_(frame_samples_ * 5) {}

  void Process(const float *data, size_t count, std::vector<float> &output) {
    output.clear();
    buffer_.insert(buffer_.end(), data, data + count);
    size_t processed = 0;
    while (buffer_.size() >= processed + frame_samples_) {
      const float *frame = buffer_.data() + processed;
      float db = FrameDb(frame);
      UpdateNoiseFloor(db);
      bool voiced = db > noise_floor_db_ + activation_margin_db_;
      if (voiced) {
        hangover_remaining_ = hangover_frames_;
        if (!history_.empty()) {
          output.insert(output.end(), history_.begin(), history_.end());
          history_.clear();
        }
      }
      if (voiced || hangover_remaining_ > 0) {
        if (!voiced) {
          --hangover_remaining_;
        }
        output.insert(output.end(), frame, frame + frame_samples_);
      } else {
        AppendHistory(frame);
      }
      processed += frame_samples_;
    }
    if (processed > 0) {
      buffer_.erase(buffer_.begin(), buffer_.begin() + processed);
    }
  }

private:
  float FrameDb(const float *frame) const {
    double sum = 0.0;
    for (int i = 0; i < frame_samples_; ++i) {
      sum += frame[i] * frame[i];
    }
    const double rms =
        std::sqrt(sum / static_cast<double>(frame_samples_) + 1e-10);
    return 20.0f * std::log10(static_cast<float>(rms) + 1e-6f);
  }

  void UpdateNoiseFloor(float db) {
    const float alpha_fast = 0.995f;
    const float alpha_slow = 0.999f;
    if (db < noise_floor_db_) {
      noise_floor_db_ = alpha_fast * noise_floor_db_ + (1.0f - alpha_fast) * db;
    } else {
      noise_floor_db_ = alpha_slow * noise_floor_db_ + (1.0f - alpha_slow) * db;
    }
    noise_floor_db_ = std::min(noise_floor_db_, -20.0f);
    noise_floor_db_ = std::max(noise_floor_db_, -80.0f);
  }

  void AppendHistory(const float *frame) {
    history_.insert(history_.end(), frame, frame + frame_samples_);
    if (history_.size() > history_limit_) {
      const size_t excess = history_.size() - history_limit_;
      history_.erase(history_.begin(), history_.begin() + excess);
    }
  }

  int frame_samples_;
  int hangover_frames_;
  int hangover_remaining_ = 0;
  size_t history_limit_;
  float noise_floor_db_ = -60.0f;
  const float activation_margin_db_ = 25.0f;
  std::vector<float> buffer_;
  std::vector<float> history_;
};

class SharedAudioBuffer {
public:
  explicit SharedAudioBuffer(size_t max_samples) : max_samples_(max_samples) {}

  void Push(const float *data, size_t count) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < count; ++i) {
      if (buffer_.size() >= max_samples_) {
        buffer_.pop_front();
      }
      buffer_.push_back(data[i]);
    }
    total_samples_ += count;
    cv_.notify_all();
  }

  bool WaitForSamples(uint64_t last_count, size_t min_new_samples) {
    std::unique_lock<std::mutex> lock(mutex_);
    cv_.wait(lock, [&] {
      return total_samples_ >= last_count + min_new_samples ||
             !g_running.load();
    });
    return total_samples_ >= last_count + min_new_samples;
  }

  std::vector<float> Latest(size_t context_samples) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const size_t available = buffer_.size();
    const size_t take = std::min(context_samples, available);
    std::vector<float> result(take);
    size_t skip = available > take ? available - take : 0;
    size_t idx = 0;
    for (float sample : buffer_) {
      if (skip > 0) {
        --skip;
        continue;
      }
      result[idx++] = sample;
    }
    return result;
  }

  uint64_t TotalSamples() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return total_samples_;
  }

  void NotifyAll() { cv_.notify_all(); }

private:
  const size_t max_samples_;
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<float> buffer_;
  uint64_t total_samples_ = 0;
};

class PortAudioCapturer {
public:
  PortAudioCapturer(SharedAudioBuffer &buffer, int preferred_device,
                    bool enable_vad)
      : buffer_(buffer), preferred_device_(preferred_device) {
    if (enable_vad) {
      vad_ = std::make_unique<VoiceActivityDetector>(kSampleRate);
    }
  }

  bool Start() {
    PaError err = Pa_Initialize();
    if (err != paNoError) {
      std::cerr << "PortAudio init failed: " << Pa_GetErrorText(err) << '\n';
      return false;
    }
    pa_initialized_ = true;

    PaStreamParameters input_params{};
    input_params.device =
        preferred_device_ >= 0 ? preferred_device_ : Pa_GetDefaultInputDevice();
    if (input_params.device == paNoDevice) {
      std::cerr << "No suitable audio input device found.\n";
      Teardown();
      return false;
    }

    const PaDeviceInfo *device_info = Pa_GetDeviceInfo(input_params.device);
    input_params.channelCount = 1;
    input_params.sampleFormat = paFloat32;
    input_params.suggestedLatency = device_info->defaultLowInputLatency;
    input_params.hostApiSpecificStreamInfo = nullptr;

    err = Pa_OpenStream(&stream_, &input_params, nullptr, kSampleRate,
                        kFramesPerBuffer, paClipOff, nullptr, nullptr);
    if (err != paNoError) {
      std::cerr << "Failed to open PortAudio stream: " << Pa_GetErrorText(err)
                << '\n';
      Teardown();
      return false;
    }

    err = Pa_StartStream(stream_);
    if (err != paNoError) {
      std::cerr << "Failed to start PortAudio stream: " << Pa_GetErrorText(err)
                << '\n';
      Teardown();
      return false;
    }

    worker_ = std::thread(&PortAudioCapturer::CaptureLoop, this);
    return true;
  }

  void Stop() { Teardown(); }

private:
  void CaptureLoop() {
    std::vector<float> block(kFramesPerBuffer);
    while (g_running.load()) {
      PaError err = Pa_ReadStream(stream_, block.data(), block.size());
      if (err == paInputOverflowed) {
        continue;
      }
      if (err == paStreamIsStopped) {
        break;
      }
      if (err != paNoError) {
        g_running.store(false);
        buffer_.NotifyAll();
        break;
      }
      if (vad_) {
        std::vector<float> voiced;
        vad_->Process(block.data(), block.size(), voiced);
        if (!voiced.empty()) {
          buffer_.Push(voiced.data(), voiced.size());
        }
      } else {
        buffer_.Push(block.data(), block.size());
      }
    }
  }

  void Teardown() {
    if (stream_) {
      Pa_StopStream(stream_);
      Pa_CloseStream(stream_);
      stream_ = nullptr;
    }
    if (worker_.joinable()) {
      worker_.join();
    }
    if (pa_initialized_) {
      Pa_Terminate();
      pa_initialized_ = false;
    }
  }

  SharedAudioBuffer &buffer_;
  int preferred_device_;
  PaStream *stream_ = nullptr;
  std::thread worker_;
  bool pa_initialized_ = false;
  std::unique_ptr<VoiceActivityDetector> vad_;
};

struct CommandLineOptions {
  std::filesystem::path model_path;
  std::filesystem::path grammar_path;
  int device_index = -1;
  int threads = std::max(1u, std::thread::hardware_concurrency());
  bool enable_vad =
      false; // DISABLED by default to prevent "waiting for speech" deadlock
};

std::optional<CommandLineOptions> ParseCommandLine(int argc, char **argv) {
  CommandLineOptions options;
  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--model" && i + 1 < argc) {
      options.model_path = argv[++i];
    } else if (arg == "--grammar" && i + 1 < argc) {
      options.grammar_path = argv[++i];
    } else if (arg == "--device" && i + 1 < argc) {
      options.device_index = std::stoi(argv[++i]);
    } else if (arg == "--threads" && i + 1 < argc) {
      options.threads = std::max(1, std::stoi(argv[++i]));
    } else if (arg == "--vad") { // Explicitly enable VAD if needed
      options.enable_vad = true;
    }
  }
  if (!std::filesystem::exists(options.model_path)) {
    std::cerr << "Model file not found: " << options.model_path << '\n';
    return std::nullopt;
  }
  return options;
}

void RecognitionLoop(whisper_context *ctx, SharedAudioBuffer &audio_buffer,
                     int num_threads, ZigbeeController &zigbee,
                     const whisper_grammar_element *grammar_rules,
                     size_t n_grammar_rules) {

  whisper_full_params params =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  params.n_threads = num_threads;
  params.audio_ctx = 0;
  params.translate = false;
  params.no_context = true;
  params.single_segment = false;
  params.print_progress = false;
  params.print_realtime = false;
  params.print_timestamps = false;
  params.language = "de";
  params.temperature = 0.f;
  params.max_tokens = 64;

  // Grammar Constraints
  if (grammar_rules != nullptr) {
    const whisper_grammar_element *rules_ptr = grammar_rules;
    params.grammar_rules = &rules_ptr;
    params.n_grammar_rules = n_grammar_rules;
    params.i_start_rule = 0;
    params.grammar_penalty = 100.0f;
  }

  // Slightly relaxed settings for "Human Level" understanding
  // We rely on our robust C++ parser to filter garbage.
  params.entropy_thold = 2.8f;   // Relaxed from 2.4f
  params.logprob_thold = -0.5f;  // Relaxed from -0.2f
  params.no_speech_thold = 0.6f; // Relaxed from 0.8f

  uint64_t last_sample_count = 0;
  std::string previous_text;

  while (g_running.load()) {
    if (!audio_buffer.WaitForSamples(last_sample_count, kChunkSamples)) {
      break;
    }
    last_sample_count = audio_buffer.TotalSamples();

    std::vector<float> audio =
        audio_buffer.Latest(static_cast<size_t>(kContextSeconds * kSampleRate));
    if (audio.empty()) {
      continue;
    }

    if (whisper_full(ctx, params, audio.data(), audio.size()) != 0) {
      std::cerr << "whisper_full failed.\n";
      continue;
    }

    const int segments = whisper_full_n_segments(ctx);
    std::string transcript;
    for (int i = 0; i < segments; ++i) {
      transcript += whisper_full_get_segment_text(ctx, i);
    }

    std::string trimmed = Trim(transcript);
    if (trimmed.empty())
      continue;

    // --- Hallucination Filter ---
    // Ignore lines containing '[' (e.g. [Musik]) or '*' or '♪'
    if (trimmed.find('[') != std::string::npos ||
        trimmed.find('*') != std::string::npos ||
        trimmed.find("♪") != std::string::npos) {
      std::cout << "[IGNORED] " << trimmed << "\r" << std::flush;
      continue;
    }

    // Verbose Logging: Print EVERYTHING we hear
    std::cout << "[HEARD] " << trimmed << "\r" << std::flush;

    // Case-insensitive deduplication
    std::string fresh = trimmed;
    std::string lower_trimmed = trimmed;
    std::string lower_previous = previous_text;
    std::transform(lower_trimmed.begin(), lower_trimmed.end(),
                   lower_trimmed.begin(), ::tolower);
    std::transform(lower_previous.begin(), lower_previous.end(),
                   lower_previous.begin(), ::tolower);

    if (!lower_previous.empty()) {
      if (lower_trimmed.rfind(lower_previous, 0) == 0) {
        fresh = trimmed.substr(previous_text.size());
      } else {
        const size_t pos = lower_trimmed.find(lower_previous);
        if (pos != std::string::npos) {
          fresh = trimmed.substr(pos + previous_text.size());
        }
      }
    }
    fresh = Trim(fresh);
    if (fresh.empty())
      continue;

    previous_text = trimmed;

    // Execute directly
    bool executed = ExecuteCommand(fresh, zigbee);
    if (executed) {
      std::cout << "\n[COMMAND] " << fresh << std::endl;
    }
  }
}

int main(int argc, char **argv) {
  auto options = ParseCommandLine(argc, argv);
  if (!options)
    return 1;

  std::signal(SIGINT, HandleSignal);
  std::signal(SIGTERM, HandleSignal);

  // Initialize MQTT
  ZigbeeController zigbee("localhost", 1883);

  whisper_context_params ctx_params = whisper_context_default_params();
  ctx_params.use_gpu = false;
  whisper_context *ctx = whisper_init_from_file_with_params(
      options->model_path.string().c_str(), ctx_params);
  if (!ctx)
    return 1;

  // Hardcoded Grammar Rules (Placeholder - using C++ Parser)
  std::vector<whisper_grammar_element> rules;

  SharedAudioBuffer audio_buffer(kHistorySamples);
  PortAudioCapturer capturer(audio_buffer, options->device_index,
                             options->enable_vad);
  if (!capturer.Start()) {
    whisper_free(ctx);
    return 1;
  }

  // Pass nullptr for grammar for now, relying on strict C++ filtering.
  std::thread recognizer(RecognitionLoop, ctx, std::ref(audio_buffer),
                         options->threads, std::ref(zigbee), nullptr, 0);

  std::cout << "ULTIMATE C++ VOICE CONTROLLER STARTED.\n";
  std::cout << "Strict Mode: Only 'Lampe [Farbe/An/Aus]' is accepted.\n";

  while (g_running.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
  }

  audio_buffer.NotifyAll();
  capturer.Stop();
  if (recognizer.joinable())
    recognizer.join();
  whisper_free(ctx);
  return 0;
}
