#include "ext.h"
#include "ext_obex.h"
#include "z_dsp.h"

#include "gst_audio_support.hpp"

#include <gst/app/gstappsrc.h>
#include <gst/gst.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kObjectName = "gst.sink~";
constexpr const char* kAppSrcName = "maxmsp_src";
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

GstCaps* make_default_appsrc_caps(long channels, double sample_rate)
{
    return gst_caps_new_simple(
        "audio/x-raw",
        "format",
        G_TYPE_STRING,
        "F32LE",
        "layout",
        G_TYPE_STRING,
        "interleaved",
        "channels",
        G_TYPE_INT,
        static_cast<int>(channels),
        "rate",
        G_TYPE_INT,
        static_cast<int>(std::llround(sample_rate)),
        nullptr);
}

GstCaps* make_appsrc_caps_from_user_caps(const std::string& caps_text, long channels, double sample_rate)
{
    if (caps_text.empty()) {
        return make_default_appsrc_caps(channels, sample_rate);
    }

    GstCaps* user_caps = gst_caps_from_string(caps_text.c_str());
    if (!user_caps) {
        return nullptr;
    }

    GstCaps* appsrc_caps = gst_caps_copy(user_caps);
    gst_caps_unref(user_caps);

    if (!appsrc_caps || gst_caps_is_empty(appsrc_caps)) {
        if (appsrc_caps) {
            gst_caps_unref(appsrc_caps);
        }
        return nullptr;
    }

    GstStructure* structure = gst_caps_get_structure(appsrc_caps, 0);
    if (!structure) {
        gst_caps_unref(appsrc_caps);
        return nullptr;
    }

    if (std::strcmp(gst_structure_get_name(structure), "audio/x-raw") != 0) {
        gst_caps_unref(appsrc_caps);
        return make_default_appsrc_caps(channels, sample_rate);
    }

    gst_structure_set(
        structure,
        "format",
        G_TYPE_STRING,
        "F32LE",
        "layout",
        G_TYPE_STRING,
        "interleaved",
        "channels",
        G_TYPE_INT,
        static_cast<int>(channels),
        "rate",
        G_TYPE_INT,
        static_cast<int>(std::llround(sample_rate)),
        nullptr);

    return appsrc_caps;
}

enum class ControlAction {
    None,
    Start,
    Stop,
    Shutdown
};

class SinkEngine {
public:
    SinkEngine(t_object* owner, long channels, method status_fn)
    : owner_(owner)
    , channels_(std::max(1L, channels))
    , sample_rate_(48000.0)
    , max_vector_size_(64)
    , running_(false)
    , exit_worker_(false)
    , next_pts_(0)
    , status_fn_(status_fn)
    {
        ring_.reset(channels_, 16384);
        worker_ = std::thread(&SinkEngine::worker_loop, this);
    }

    ~SinkEngine()
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

    void perform(double** ins, long sampleframes)
    {
        if (!running_.load(std::memory_order_relaxed) || !ins || sampleframes <= 0) {
            return;
        }

        ring_.push_from_deinterleaved(ins, static_cast<std::size_t>(sampleframes));
        audio_ready_.store(true, std::memory_order_release);
        worker_cv_.notify_one();
    }

    void clear()
    {
        ring_.clear();
    }

    long channel_count() const
    {
        return channels_;
    }

    bool start()
    {
        std::string pipeline_tail;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            pipeline_tail = trim_copy(user_pipeline_);
        }

        if (pipeline_tail.empty()) {
            post_status("error", "set a pipeline first with the 'pipeline' message");
            return false;
        }

        request_action(ControlAction::Start);
        return true;
    }

    void stop()
    {
        request_action(ControlAction::Stop);
    }

    // ワーカースレッドから defer_low 経由でアウトレットに安全に出力する
    void post_status(const char* sel, const char* msg = nullptr)
    {
        if (!status_fn_) {
            return;
        }
        if (msg) {
            t_atom atom;
            atom_setsym(&atom, gensym(msg));
            defer_low(owner_, status_fn_, gensym(sel), 1, &atom);
        }
        else {
            defer_low(owner_, status_fn_, gensym(sel), 0, nullptr);
        }
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
        std::string pipeline_tail;
        std::string caps_text;
        {
            std::lock_guard<std::mutex> lock(config_mutex_);
            pipeline_tail = trim_copy(user_pipeline_);
            caps_text = trim_copy(user_caps_);
        }

        if (pipeline_tail.empty()) {
            post_status("error", "set a pipeline first with the 'pipeline' message");
            return false;
        }

        stop_internal(false);
        ensure_gstreamer();

        std::string pipeline_text =
            "appsrc name=" + std::string(kAppSrcName) +
            " ! queue leaky=2 max-size-buffers=64 max-size-time=0 max-size-bytes=0 ! audioconvert ! audioresample";

        if (!caps_text.empty()) {
            pipeline_text += " ! capsfilter caps=\"" + caps_text + "\"";
        }

        pipeline_text += " ! " + pipeline_tail;

        GError* error = nullptr;
        GstElement* pipeline = gst_parse_launch(pipeline_text.c_str(), &error);

        if (!pipeline || error) {
            post_status("error", error ? error->message : "could not parse pipeline");
            if (error) {
                g_error_free(error);
            }
            if (pipeline) {
                gst_object_unref(pipeline);
            }
            return false;
        }

        GstElement* appsrc_element = gst_bin_get_by_name(GST_BIN(pipeline), kAppSrcName);
        if (!appsrc_element || !GST_IS_APP_SRC(appsrc_element)) {
            post_status("error", "internal appsrc could not be created");
            if (appsrc_element) {
                gst_object_unref(appsrc_element);
            }
            gst_object_unref(pipeline);
            return false;
        }

        auto* appsrc = GST_APP_SRC(appsrc_element);
        g_object_set(G_OBJECT(appsrc), "format", GST_FORMAT_TIME, "is-live", TRUE, "do-timestamp", TRUE, "block", FALSE, nullptr);
        gst_app_src_set_stream_type(appsrc, GST_APP_STREAM_TYPE_STREAM);
        gst_app_src_set_max_buffers(appsrc, 64);
        gst_app_src_set_leaky_type(appsrc, GST_APP_LEAKY_TYPE_DOWNSTREAM);

        GstCaps* appsrc_caps = make_appsrc_caps_from_user_caps(caps_text, channels_, sample_rate_.load());
        if (!appsrc_caps) {
            post_status("error", "invalid audio caps");
            gst_object_unref(appsrc_element);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }

        gst_app_src_set_caps(appsrc, appsrc_caps);
        gst_caps_unref(appsrc_caps);

        const auto state_result = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (state_result == GST_STATE_CHANGE_FAILURE) {
            post_status("error", "pipeline failed to enter PLAYING state");
            gst_object_unref(appsrc_element);
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
            return false;
        }

        if (state_result == GST_STATE_CHANGE_ASYNC) {
            const auto resolved = gst_element_get_state(pipeline, nullptr, nullptr, 5 * GST_SECOND);
            if (resolved == GST_STATE_CHANGE_FAILURE) {
                post_status("error", "pipeline did not finish starting");
                gst_object_unref(appsrc_element);
                gst_element_set_state(pipeline, GST_STATE_NULL);
                gst_object_unref(pipeline);
                return false;
            }
        }

        ring_.clear();

        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            pipeline_ = pipeline;
            appsrc_ = appsrc;
            bus_ = gst_element_get_bus(pipeline);
            next_pts_ = 0;
            running_.store(true);
        }

        post_status("started");
        return true;
    }

    void stop_internal(bool announce)
    {
        GstElement* pipeline = nullptr;
        GstAppSrc* appsrc = nullptr;
        GstBus* bus = nullptr;

        {
            std::lock_guard<std::mutex> lock(pipeline_mutex_);
            if (!pipeline_ && !appsrc_) {
                running_.store(false);
                ring_.clear();
                return;
            }

            running_.store(false);
            pipeline = pipeline_;
            appsrc = appsrc_;
            bus = bus_;
            pipeline_ = nullptr;
            appsrc_ = nullptr;
            bus_ = nullptr;
            next_pts_ = 0;
        }

        ring_.clear();

        if (appsrc) {
            // EOS を送らずそのまま unref: EOS のダウンストリーム伝播が
            // 後の状態遷移と競合してネットワークリソースの二重解放を引き起こすため
            gst_object_unref(appsrc);
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
                post_status("stopped");
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
                post_status("error", error ? error->message : "unknown");
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
                post_status("warning", error ? error->message : "unknown");
                if (error) {
                    g_error_free(error);
                }
                if (debug) {
                    g_free(debug);
                }
                break;
            }
            case GST_MESSAGE_EOS:
                post_status("eos");
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
                // 旧条件の running_.load() を除去: running_ が true のとき wait が即リターンし
                // ビジースピンになっていた。audio_ready_ フラグで perform() からの通知を受ける。
                worker_cv_.wait_for(lock, std::chrono::milliseconds(50), [this]() {
                    return exit_worker_.load()
                        || pending_action_ != ControlAction::None
                        || audio_ready_.load(std::memory_order_relaxed);
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

            // バスポーリングを ~10Hz に制限（毎 DSP ベクター呼び出しを避ける）
            const auto now = std::chrono::steady_clock::now();
            if (now - last_bus_pump >= std::chrono::milliseconds(100)) {
                pump_bus();
                last_bus_pump = now;
            }

            if (!running_.load()) {
                continue;
            }

            // フラグをクリアしてからリングバッファを読む
            audio_ready_.store(false, std::memory_order_relaxed);

            const auto frames_available = ring_.size_frames();
            if (frames_available == 0) {
                continue;
            }

            const auto chunk_frames = std::min<std::size_t>(
                frames_available,
                static_cast<std::size_t>(std::max(1L, max_vector_size_.load()) * 8));

            const auto needed = chunk_frames * static_cast<std::size_t>(channels_);
            if (scratch_.size() < needed) {
                scratch_.resize(needed);
            }

            const auto pulled_frames = ring_.pop_interleaved(scratch_.data(), chunk_frames);
            if (pulled_frames == 0) {
                continue;
            }

            // まだリングにデータが残っていれば次イテレーションへのフラグを立て直す
            if (ring_.size_frames() > 0) {
                audio_ready_.store(true, std::memory_order_relaxed);
            }

            GstAppSrc* appsrc = nullptr;
            GstClockTime pts = 0;
            GstClockTime duration = 0;

            {
                std::lock_guard<std::mutex> lock(pipeline_mutex_);
                if (!running_.load() || !appsrc_) {
                    continue;
                }

                appsrc = GST_APP_SRC(gst_object_ref(appsrc_));
                pts = next_pts_;
                duration = gstmax::frames_to_clock_time(pulled_frames, sample_rate_.load());
                next_pts_ += duration;
            }

            GstBuffer* buffer = gst_buffer_new_allocate(
                nullptr,
                static_cast<gsize>(pulled_frames * static_cast<std::size_t>(channels_) * sizeof(float)),
                nullptr);

            if (!buffer) {
                gst_object_unref(appsrc);
                continue;
            }

            GstMapInfo map;
            if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
                gst_buffer_unref(buffer);
                gst_object_unref(appsrc);
                continue;
            }

            std::memcpy(map.data, scratch_.data(), map.size);
            gst_buffer_unmap(buffer, &map);

            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DURATION(buffer) = duration;

            const auto flow = gst_app_src_push_buffer(appsrc, buffer);
            gst_object_unref(appsrc);

            if (flow == GST_FLOW_FLUSHING || flow == GST_FLOW_EOS) {
                continue;
            }
        }

        // ループを exit_worker_ の条件チェックで抜けた場合にも確実にパイプラインを停止する。
        // Shutdown アクション処理のパスでは stop_internal + break 済みだが、
        // gst_app_src_push_buffer 中に exit_worker_ が立った場合は
        // while 条件でループを抜けるため stop_internal が呼ばれない。
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
    GstAppSrc* appsrc_ { nullptr };
    GstBus* bus_ { nullptr };
    std::mutex worker_mutex_;
    std::condition_variable worker_cv_;
    std::thread worker_;
    std::atomic<bool> running_;
    std::atomic<bool> exit_worker_;
    std::atomic<bool> audio_ready_ { false };
    ControlAction pending_action_ { ControlAction::None };
    GstClockTime next_pts_;
    std::vector<float> scratch_;
    method status_fn_;
};

typedef struct _gst_sink {
    t_pxobject ob;
    SinkEngine* engine;
    void* outlet_status;
} t_gst_sink;

t_class* gst_sink_class = nullptr;

void gst_sink_output_status(t_gst_sink* x, t_symbol* sym, short argc, t_atom* argv)
{
    outlet_anything(x->outlet_status, sym, argc, argv);
}

void gst_sink_assist(t_gst_sink* x, void* b, long m, long a, char* s)
{
    if (m == ASSIST_INLET) {
        const auto channel_count = x->engine ? x->engine->channel_count() : 0;
        snprintf(s, 512, "Signal inlet %ld of %ld", a + 1, channel_count);
    }
    else {
        snprintf(s, 512, "Status: started / stopped / error <msg> / warning <msg> / eos");
    }
}

void gst_sink_pipeline(t_gst_sink* x, t_symbol* s, long argc, t_atom* argv)
{
    if (x->engine) {
        x->engine->set_pipeline(gstmax::atoms_to_string(argc, argv));
    }
}

void gst_sink_caps(t_gst_sink* x, t_symbol* s, long argc, t_atom* argv)
{
    if (x->engine) {
        x->engine->set_caps(gstmax::atoms_to_string(argc, argv));
    }
}

void gst_sink_start(t_gst_sink* x)
{
    if (x->engine) {
        x->engine->start();
    }
}

void gst_sink_stop(t_gst_sink* x)
{
    if (x->engine) {
        x->engine->stop();
    }
}

void gst_sink_clear(t_gst_sink* x)
{
    if (x->engine) {
        x->engine->clear();
    }
}

void gst_sink_perform64(t_gst_sink* x, t_object* dsp64, double** ins, long numins, double** outs, long numouts, long sampleframes, long flags, void* userparam)
{
    if (x->engine) {
        x->engine->perform(ins, sampleframes);
    }
}

void gst_sink_dsp64(t_gst_sink* x, t_object* dsp64, short* count, double samplerate, long maxvectorsize, long flags)
{
    if (x->engine) {
        x->engine->prepare(samplerate, maxvectorsize);
    }

    object_method(dsp64, gensym("dsp_add64"), x, gst_sink_perform64, 0, nullptr);
}

void gst_sink_free(t_gst_sink* x)
{
    dsp_free((t_pxobject*)x);
    delete x->engine;
}

void* gst_sink_new(t_symbol* s, long argc, t_atom* argv)
{
    long consumed = 0;
    const auto channels = gstmax::parse_channel_argument(argc, argv, kDefaultChannels, &consumed);

    auto* x = static_cast<t_gst_sink*>(object_alloc(gst_sink_class));
    if (!x) {
        return nullptr;
    }

    dsp_setup((t_pxobject*)x, channels);
    x->outlet_status = outlet_new((t_object*)x, nullptr);
    x->engine = new SinkEngine((t_object*)x, channels, (method)gst_sink_output_status);

    if (argc > consumed) {
        x->engine->set_pipeline(gstmax::atoms_to_string(argc - consumed, argv + consumed));
    }

    return x;
}

} // namespace

C74_EXPORT void ext_main(void* r)
{
    ensure_gstreamer();

    t_class* c = class_new(kObjectName, (method)gst_sink_new, (method)gst_sink_free, sizeof(t_gst_sink), 0L, A_GIMME, 0);

    class_addmethod(c, (method)gst_sink_pipeline, "pipeline", A_GIMME, 0);
    class_addmethod(c, (method)gst_sink_caps, "caps", A_GIMME, 0);
    class_addmethod(c, (method)gst_sink_start, "start", 0);
    class_addmethod(c, (method)gst_sink_stop, "stop", 0);
    class_addmethod(c, (method)gst_sink_clear, "clear", 0);
    class_addmethod(c, (method)gst_sink_assist, "assist", A_CANT, 0);
    class_addmethod(c, (method)gst_sink_dsp64, "dsp64", A_CANT, 0);

    class_dspinit(c);
    class_register(CLASS_BOX, c);
    gst_sink_class = c;
}
