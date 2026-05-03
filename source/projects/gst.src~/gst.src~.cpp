#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

#include "gst_audio_support.hpp"

#include <gst/app/gstappsink.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>

namespace {

constexpr const char* kObjectName = "gst.src~";
constexpr const char* kAppSinkName = "maxmsp_sink";
constexpr long kDefaultChannels = 2;

std::string trim_copy(const std::string& value)
{
    const auto begin = value.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }

    const auto end = value.find_last_not_of(" \t\r\n");
    return value.substr(begin, end - begin + 1);
}

void ensure_gstreamer()
{
    static std::once_flag once;
    std::call_once(once, []() { gst_init(nullptr, nullptr); });
}

enum class ControlAction {
    None,
    Start,
    Stop,
    Shutdown
};

class SourceEngine {
public:
    SourceEngine(t_object* owner, long channels)
    : owner_(owner)
    , channels_(std::max(1L, channels))
    , sample_rate_(48000.0)
    , max_vector_size_(64)
    , running_(false)
    , exit_worker_(false)
    {
        ring_.reset(channels_, 16384);
        worker_ = std::thread(&SourceEngine::worker_loop, this);
    }

    ~SourceEngine()
    {
        shutdown();
    }

    void set_pipeline(const std::string& pipeline)
    {
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            user_pipeline_ = gstmax::unescape_max_text(trim_copy(pipeline));
        }

        if (running_.load()) {
            start();
        }
    }

    void set_caps(const std::string& caps_text)
    {
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            user_caps_ = gstmax::unescape_max_text(trim_copy(caps_text));
        }

        if (running_.load()) {
            start();
        }
    }

    void prepare(double sample_rate, long max_vector_size)
    {
        const auto safe_rate = sample_rate > 0.0 ? sample_rate : 48000.0;
        const auto safe_vector = std::max(1L, max_vector_size);
        const auto current_rate = sample_rate_.load();
        const auto current_vector = max_vector_size_.load();

        sample_rate_.store(safe_rate);
        max_vector_size_.store(safe_vector);

        const auto quarter_second = static_cast<std::size_t>(std::llround(safe_rate * 0.25));
        const auto capacity = std::max<std::size_t>(static_cast<std::size_t>(safe_vector) * 256, quarter_second);
        ring_.reset(channels_, capacity);

        if (running_.load() && (std::fabs(current_rate - safe_rate) > 1.0 || current_vector != safe_vector)) {
            start();
        }
    }

    void perform(double** outs, long sampleframes)
    {
        if (!outs || sampleframes <= 0) {
            return;
        }

        if (!running_.load()) {
            for (long channel = 0; channel < channels_; ++channel) {
                std::fill(outs[channel], outs[channel] + sampleframes, 0.0);
            }
            return;
        }

        ring_.pop_to_deinterleaved(outs, static_cast<std::size_t>(sampleframes));
    }

    void clear()
    {
        ring_.clear();
    }

    bool start()
    {
        std::string pipeline_head;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            pipeline_head = trim_copy(user_pipeline_);
        }

        if (pipeline_head.empty()) {
            object_error(owner_, "%s: set a pipeline first with the 'pipeline' message", kObjectName);
            return false;
        }

        request_action(ControlAction::Start);
        return true;
    }

    void stop()
    {
        request_action(ControlAction::Stop);
    }

private:
    void request_action(ControlAction action)
    {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            if (pending_action_ == ControlAction::Shutdown) {
                return;
            }
            pending_action_ = action;
        }

        worker_cv_.notify_all();
    }

    bool start_internal()
    {
        std::string pipeline_head;
        std::string caps_text;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            pipeline_head = trim_copy(user_pipeline_);
            caps_text = trim_copy(user_caps_);
        }

        if (pipeline_head.empty()) {
            object_error(owner_, "%s: set a pipeline first with the 'pipeline' message", kObjectName);
            return false;
        }

        stop_internal(false);
        ensure_gstreamer();

        const std::string pipeline_text =
            pipeline_head +
            " ! audioconvert ! audioresample ! queue leaky=2 max-size-buffers=64 max-size-time=0 max-size-bytes=0 ! appsink name=" +
            std::string(kAppSinkName);

        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(pipeline_text.c_str(), &error);

        if (!pipeline || error) {
            object_error(owner_, "%s: could not parse pipeline: %s", kObjectName, error ? error->message : "unknown error");
            if (error) {
                g_error_free(error);
            }
            if (pipeline) {
                gst_object_unref(pipeline);
            }
            return false;
        }

        GstElement* appsink_element = gst_bin_get_by_name(GST_BIN(pipeline), kAppSinkName);
        if (!appsink_element || !GST_IS_APP_SINK(appsink_element)) {
            object_error(owner_, "%s: internal appsink could not be created", kObjectName);
            if (appsink_element) {
                gst_object_unref(appsink_element);
            }
            gst_object_unref(pipeline);
            return false;
        }

        auto* appsink = GST_APP_SINK(appsink_element);
        g_object_set(
            G_OBJECT(appsink),
            "sync",
            FALSE,
            "drop",
            TRUE,
            "max-buffers",
            64u,
            "emit-signals",
            FALSE,
            "wait-on-eos",
            FALSE,
            nullptr);

        GstCaps* caps = nullptr;
        if (!caps_text.empty()) {
            caps = gst_caps_from_string(caps_text.c_str());
            if (!caps) {
                object_error(owner_, "%s: invalid caps string: %s", kObjectName, caps_text.c_str());
                gst_object_unref(appsink_element);
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                return false;
            }
        }
        else {
            caps = gst_caps_new_simple(
                "audio/x-raw",
                "format",
                G_TYPE_STRING,
                "F32LE",
                "layout",
                G_TYPE_STRING,
                "interleaved",
                "channels",
                G_TYPE_INT,
                static_cast<int>(channels_),
                "rate",
                G_TYPE_INT,
                static_cast<int>(std::llround(sample_rate_.load())),
                nullptr);
        }
        gst_app_sink_set_caps(appsink, caps);
        gst_caps_unref(caps);

        const auto state_result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            object_error(owner_, "%s: pipeline failed to enter PLAYING state", kObjectName);
            gst_object_unref(appsink_element);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }

        if (state_result == GST_STATE_CHANGE_ASYNC) {
            const auto resolved = gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
            if (resolved == GST_STATE_CHANGE_FAILURE) {
                object_error(owner_, "%s: pipeline did not finish starting", kObjectName);
                gst_object_unref(appsink_element);
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                return false;
            }
        }

        ring_.clear();

        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            pipeline_ = pipeline;
            appsink_ = appsink;
            bus_ = gst_element_get_bus(pipeline);
            running_.store(true);
        }

        object_post(owner_, "%s: started", kObjectName);
        return true;
    }

    void stop_internal(bool announce)
    {
        GstElement* pipeline = nullptr;
        GstAppSink* appsink = nullptr;
        GstBus* bus = nullptr;

        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            if (!pipeline_ && !appsink_) {
                running_.store(false);
                ring_.clear();
                return;
            }

            running_.store(false);
            pipeline = pipeline_;
            appsink = appsink_;
            bus = bus_;
            pipeline_ = nullptr;
            appsink_ = nullptr;
            bus_ = nullptr;
        }

        ring_.clear();

        if (appsink) {
            gst_object_unref(appsink);
        }

        if (pipeline) {
            // GStreamer 推奨の停止手順: PLAYING→PAUSED→NULL を段階的に踏む。
            // PLAYING→PAUSED は非同期のため必ず完了を待ってから NULL へ進む。
            // これにより各要素がソケット等のネットワークリソースを確実に解放する。
            gst_element_set_state(pipeline, GST_STATE_PAUSED);
            gst_element_get_state(pipeline, nullptr, nullptr, 3 * GST_SECOND);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_element_get_state(pipeline, nullptr, nullptr, 3 * GST_SECOND);
            gst_object_unref(pipeline);
            if (announce) {
                object_post(owner_, "%s: stopped", kObjectName);
            }
        }

        if (bus) {
            gst_object_unref(bus);
        }
    }

    void pump_bus()
    {
        GstBus* bus = nullptr;
        bool should_stop = false;
        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            if (bus_) {
                bus = GST_BUS(gst_object_ref(bus_));
            }
        }

        if (!bus) {
            return;
        }

        while (GstMessage* message = gst_bus_pop_filtered(bus, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS))) {
            switch (GST_MESSAGE_TYPE(message)) {
            case GST_MESSAGE_ERROR: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_error(message, &error, &debug);
                object_error(owner_, "%s bus ERROR: %s | dbg: %s", kObjectName, error ? error->message : "unknown", debug ? debug : "none");
                if (error) {
                    g_error_free(error);
                }
                if (debug) {
                    g_free(debug);
                }
                should_stop = true;
                break;
            }
            case GST_MESSAGE_WARNING: {
                GError* error = nullptr;
                gchar* debug = nullptr;
                gst_message_parse_warning(message, &error, &debug);
                object_post(owner_, "%s bus WARNING: %s | dbg: %s", kObjectName, error ? error->message : "unknown", debug ? debug : "none");
                if (error) {
                    g_error_free(error);
                }
                if (debug) {
                    g_free(debug);
                }
                break;
            }
            case GST_MESSAGE_EOS:
                object_post(owner_, "%s bus EOS", kObjectName);
                should_stop = true;
                break;
            default:
                break;
            }

            gst_message_unref(message);
        }

        gst_object_unref(bus);

        if (should_stop) {
            stop_internal(true);
        }
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock(worker_mutex_);
            exit_worker_.store(true);
            pending_action_ = ControlAction::Shutdown;
        }

        worker_cv_.notify_all();

        if (worker_.joinable()) {
            worker_.join();
        }
    }

    void worker_loop()
    {
        auto last_bus_pump = std::chrono::steady_clock::now();

        while (!exit_worker_.load()) {
            ControlAction action = ControlAction::None;
            {
                std::unique_lock<std::mutex> lock(worker_mutex_);
                worker_cv_.wait_for(lock, std::chrono::milliseconds(10), [this]() {
                    return exit_worker_.load() || pending_action_ != ControlAction::None || running_.load();
                });
                action = pending_action_;
                pending_action_ = ControlAction::None;
            }

            if (action == ControlAction::Shutdown || exit_worker_.load()) {
                stop_internal(false);
                break;
            }

            if (action == ControlAction::Stop) {
                stop_internal(true);
                continue;
            }

            if (action == ControlAction::Start) {
                start_internal();
                continue;
            }

            if (!running_.load()) {
                continue;
            }

            // バスポーリングを ~10Hz に制限（try_pull_sample が100Hz でループするため
            // 毎回呼ぶと3600回/秒 × インスタンス数のオーバーヘッドになる）
            const auto now = std::chrono::steady_clock::now();
            if (now - last_bus_pump >= std::chrono::milliseconds(100)) {
                pump_bus();
                last_bus_pump = now;
            }

            if (!running_.load()) {
                continue;
            }

            GstAppSink* appsink = nullptr;
            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                if (!running_.load() || !appsink_) {
                    continue;
                }

                appsink = GST_APP_SINK(gst_object_ref(appsink_));
            }

            GstSample* sample = gst_app_sink_try_pull_sample(appsink, 10 * GST_MSECOND);
            gst_object_unref(appsink);

            if (!sample) {
                continue;
            }

            GstBuffer* buffer = gst_sample_get_buffer(sample);
            if (!buffer) {
                gst_sample_unref(sample);
                continue;
            }

            GstMapInfo map;
            if (!gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                gst_sample_unref(sample);
                continue;
            }

            const auto frame_count = map.size / (static_cast<std::size_t>(channels_) * sizeof(float));
            ring_.push_interleaved(reinterpret_cast<const float*>(map.data), frame_count);

            gst_buffer_unmap(buffer, &map);
            gst_sample_unref(sample);
        }

        // ループを exit_worker_ の条件チェックで抜けた場合にも確実にパイプラインを停止する。
        // gst_app_sink_try_pull_sample(10ms) でブロック中に exit_worker_ が立つと、
        // タイムアウト後に while 条件でループを抜けて Shutdown アクションハンドラを
        // 通らないため stop_internal が呼ばれず、udpsrc 等のポートが解放されない。
        stop_internal(false);
    }

    t_object* owner_;
    const long channels_;
    std::atomic<double> sample_rate_;
    std::atomic<long> max_vector_size_;
    gstmax::InterleavedRingBuffer ring_;
    std::mutex config_mutex_;
    std::string user_pipeline_;
    std::string user_caps_;
    std::mutex pipeline_mutex_;
    GstElement* pipeline_ { nullptr };
    GstAppSink* appsink_ { nullptr };
    GstBus* bus_ { nullptr };
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::thread worker_;
    std::atomic<bool> running_;
    std::atomic<bool> exit_worker_;
    ControlAction pending_action_ { ControlAction::None };
};

typedef struct _gst_src {
    t_pxobject ob;
    SourceEngine* engine;
    long channels;
} t_gst_src;

t_class* gst_src_class = nullptr;

void gst_src_assist(t_gst_src* x, void* b, long m, long a, char* s)
{
    if (m == ASSIST_INLET) {
        snprintf(s, 512, "No signal inlets");
    }
    else {
        snprintf(s, 512, "Signal outlet %ld of %ld", a + 1, x->channels);
    }
}

void gst_src_pipeline(t_gst_src* x, t_symbol* s, long argc, t_atom* argv)
{
    if (x->engine) {
        x->engine->set_pipeline(gstmax::atoms_to_string(argc, argv));
    }
}

void gst_src_caps(t_gst_src* x, t_symbol* s, long argc, t_atom* argv)
{
    if (x->engine) {
        x->engine->set_caps(gstmax::atoms_to_string(argc, argv));
    }
}

void gst_src_start(t_gst_src* x)
{
    if (x->engine) {
        x->engine->start();
    }
}

void gst_src_stop(t_gst_src* x)
{
    if (x->engine) {
        x->engine->stop();
    }
}

void gst_src_clear(t_gst_src* x)
{
    if (x->engine) {
        x->engine->clear();
    }
}

void gst_src_perform64(t_gst_src* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam)
{
    if (x->engine) {
        x->engine->perform(outs, sampleframes);
    }
}

void gst_src_dsp64(t_gst_src* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags)
{
    if (x->engine) {
        x->engine->prepare(samplerate, maxvectorsize);
    }

    object_method(dsp64, gensym("dsp_add64"), x, gst_src_perform64, 0, nullptr);
}

void gst_src_free(t_gst_src* x)
{
    dsp_free((t_pxobject*)x);
    delete x->engine;
}

void* gst_src_new(t_symbol* s, long argc, t_atom* argv)
{
    long consumed = 0;
    const auto channels = gstmax::parse_channel_argument(argc, argv, kDefaultChannels, &consumed);

    auto* x = static_cast<t_gst_src*>(object_alloc(gst_src_class));
    if (!x) {
        return nullptr;
    }

    dsp_setup((t_pxobject*)x, 0);
    x->channels = channels;
    x->engine = new SourceEngine((t_object*)x, channels);

    for (long channel = 0; channel < channels; ++channel) {
        outlet_new((t_object*)x, "signal");
    }

    if (argc > consumed) {
        x->engine->set_pipeline(gstmax::atoms_to_string(argc - consumed, argv + consumed));
    }

    return x;
}

} // namespace

C74_EXPORT void ext_main(void* r)
{
    ensure_gstreamer();

    t_class* c = class_new(kObjectName, (method)gst_src_new, (method)gst_src_free, sizeof(t_gst_src), 0L, A_GIMME, 0);

    class_addmethod(c, (method)gst_src_pipeline, "pipeline", A_GIMME, 0);
    class_addmethod(c, (method)gst_src_caps, "caps", A_GIMME, 0);
    class_addmethod(c, (method)gst_src_start, "start", 0);
    class_addmethod(c, (method)gst_src_stop, "stop", 0);
    class_addmethod(c, (method)gst_src_clear, "clear", 0);
    class_addmethod(c, (method)gst_src_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)gst_src_dsp64, "dsp64", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    gst_src_class = c;
}
