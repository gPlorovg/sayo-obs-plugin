#include <obs-module.h>
#include <obs.h>
#include <obs-data.h>
#include <obs-properties.h>
#include <plugin-support.h>
#include <samplerate.h>
#include <util/platform.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "grpc_transport.h"
#include "settings_model.h"
#include "subtitle_buffer.h"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {
std::atomic<int> asr_instance_counter{0};
constexpr int kResamplerWarmupFrames = 5;
} // namespace

struct SourceContext {
	obs_source_t *source = nullptr;
	obs_source_t *internal_text_source = nullptr;

	std::mutex grpc_mutex;
	std::unique_ptr<ASRGrpcClient> grpc_client;
	std::atomic<bool> streaming_active{false};
	std::atomic<bool> grpc_handshake_pending{false};
	std::vector<sayo::ModelDescriptor> available_models;
	std::string connect_status = "Unknown";
	std::atomic<bool> connect_in_progress{false};
	std::atomic<bool> connect_pending_apply{false};
	std::atomic<bool> shutting_down{false};
	std::thread connect_thread;

	sayo_plugin::RuntimeSettings settings;
	SubtitlesBuffer subtitles_buffer{2, 60};
	std::mutex subtitle_mutex;
	std::string cached_user_text;
	bool asr_received_content = false;
	std::string pending_display_text;
	std::string last_applied_text;

	/* Subtitle logging */
	bool subtitle_log_enabled = false;
	std::ofstream subtitle_log_stream;
	std::string subtitle_log_last_final;

	SRC_STATE *resampler = nullptr;
	uint32_t input_sample_rate = 48000;
	float resample_ratio = 16000.0f / 48000.0f;
	std::vector<float> resample_output_buffer;
	std::vector<char> send_buffer;
	size_t audio_chunk_size_bytes = 16000 / 10 * sizeof(float);
	int resampler_warmup = kResamplerWarmupFrames;
};

static const char *asr_get_name(void *)
{
	return "Sayo ASR Text Source";
}

static std::string sanitize_filename(std::string s)
{
	for (char &c : s) {
		const unsigned char uc = static_cast<unsigned char>(c);
		/* Only sanitize ASCII control/reserved characters; keep UTF-8 bytes intact. */
		const bool bad = (uc < 0x20) || c == '<' || c == '>' || c == ':' || c == '"' || c == '/' || c == '\\' ||
		                 c == '|' || c == '?' || c == '*';
		if (bad)
			c = '_';
	}
	if (s.empty()) {
		return "source";
	}
	return s;
}

static std::string now_stamp_for_filename()
{
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y-%m-%d_%H-%M-%S");
	return oss.str();
}

static std::string now_stamp_for_log()
{
	const std::time_t t = std::time(nullptr);
	std::tm tm{};
#ifdef _WIN32
	localtime_s(&tm, &t);
#else
	localtime_r(&t, &tm);
#endif
	std::ostringstream oss;
	oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
	return oss.str();
}

static std::string obs_data_to_json_with_defaults_or_empty(obs_data_t *data)
{
	if (!data) {
		return {};
	}
#ifdef _WIN32
	const char *json = obs_data_get_json_with_defaults(data);
#else
	const char *json = obs_data_get_json(data);
#endif
	return json ? std::string(json) : std::string{};
}

static bool noop_info_button(obs_properties_t *, obs_property_t *, void *)
{
	return true;
}

static void add_info_text(obs_properties_t *props, const char *name, const char *text, bool fallback_as_button = false)
{
#ifdef OBS_TEXT_INFO
	obs_properties_add_text(props, name, text, OBS_TEXT_INFO);
#else
	if (fallback_as_button) {
		obs_property_t *prop = obs_properties_add_button(props, name, text, noop_info_button);
		if (prop) {
			obs_property_set_enabled(prop, false);
		}
	} else {
		(void)props;
		(void)name;
		(void)text;
	}
#endif
}

static void obs_data_copy_item_with_defaults(obs_data_t *dst, obs_data_item_t *item)
{
	if (!dst || !item) {
		return;
	}

	const char *name = obs_data_item_get_name(item);
	if (!name || !*name) {
		return;
	}

	const enum obs_data_type t = obs_data_item_gettype(item);
	switch (t) {
	case OBS_DATA_STRING:
		obs_data_set_string(dst, name, obs_data_item_get_string(item));
		if (obs_data_item_has_default_value(item))
			obs_data_set_default_string(dst, name, obs_data_item_get_default_string(item));
		break;
	case OBS_DATA_NUMBER: {
		const enum obs_data_number_type nt = obs_data_item_numtype(item);
		if (nt == OBS_DATA_NUM_DOUBLE) {
			obs_data_set_double(dst, name, obs_data_item_get_double(item));
			if (obs_data_item_has_default_value(item))
				obs_data_set_default_double(dst, name, obs_data_item_get_default_double(item));
		} else {
			obs_data_set_int(dst, name, obs_data_item_get_int(item));
			if (obs_data_item_has_default_value(item))
				obs_data_set_default_int(dst, name, obs_data_item_get_default_int(item));
		}
		break;
	}
	case OBS_DATA_BOOLEAN:
		obs_data_set_bool(dst, name, obs_data_item_get_bool(item));
		if (obs_data_item_has_default_value(item))
			obs_data_set_default_bool(dst, name, obs_data_item_get_default_bool(item));
		break;
	case OBS_DATA_OBJECT: {
		obs_data_t *obj = obs_data_item_get_obj(item);
		if (obj)
			obs_data_set_obj(dst, name, obj);
		if (obs_data_item_has_default_value(item)) {
			obs_data_t *dobj = obs_data_item_get_default_obj(item);
			if (dobj)
				obs_data_set_default_obj(dst, name, dobj);
		}
		break;
	}
	case OBS_DATA_ARRAY: {
		obs_data_array_t *arr = obs_data_item_get_array(item);
		if (arr)
			obs_data_set_array(dst, name, arr);
		if (obs_data_item_has_default_value(item)) {
			obs_data_array_t *darr = obs_data_item_get_default_array(item);
			if (darr)
				obs_data_set_default_array(dst, name, darr);
		}
		break;
	}
	case OBS_DATA_NULL:
	default:
		break;
	}
}

static std::string obs_data_to_json_with_defaults_filtered_by_keys(obs_data_t *settings, const char *const *keys)
{
	if (!settings || !keys) {
		return {};
	}
	obs_data_t *filtered = obs_data_create();
	for (const char *const *k = keys; *k; ++k) {
		obs_data_item_t *it = obs_data_item_byname(settings, *k);
		if (!it) {
			continue;
		}
		obs_data_copy_item_with_defaults(filtered, it);
		obs_data_item_release(&it);
	}
	const std::string json = obs_data_to_json_with_defaults_or_empty(filtered);
	obs_data_release(filtered);
	return json;
}

static std::string text_source_settings_json_filtered(obs_source_t *text_source)
{
	if (!text_source) {
		return {};
	}

	obs_data_t *settings = obs_source_get_settings(text_source);
	obs_properties_t *props = obs_source_properties(text_source);
	if (!settings || !props) {
		if (props)
			obs_properties_destroy(props);
		if (settings)
			obs_data_release(settings);
		return {};
	}

	obs_data_t *filtered = obs_data_create();
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		const char *name = obs_property_name(p);
		if (!name || !*name) {
			continue;
		}
		/* Text content is logged below; keep meta stable. */
		if (std::strcmp(name, "text") == 0) {
			continue;
		}
		/* Hide-only options: do not log them in meta */
		if (std::strcmp(name, "from_file") == 0 || std::strcmp(name, "log_lines") == 0 ||
		    std::strcmp(name, "custom_width") == 0 || std::strcmp(name, "word_wrap") == 0) {
			continue;
		}
		obs_data_item_t *it = obs_data_item_byname(settings, name);
		if (it) {
			obs_data_copy_item_with_defaults(filtered, it);
			obs_data_item_release(&it);
		}
	}

	const std::string json = obs_data_to_json_with_defaults_or_empty(filtered);

	obs_data_release(filtered);
	obs_properties_destroy(props);
	obs_data_release(settings);
	return json;
}

static std::filesystem::path build_subtitle_log_path(obs_source_t *source)
{
	char *dir = os_get_config_path_ptr("obs-studio/logs");
	std::string out_dir = dir ? dir : "";
	bfree(dir);
	if (!out_dir.empty()) {
		os_mkdirs(out_dir.c_str());
	}

	const char *src_name = source ? obs_source_get_name(source) : "source";
	const std::string file = sanitize_filename(src_name ? src_name : "source") + "_" + now_stamp_for_filename() + ".txt";
	if (out_dir.empty()) {
		return std::filesystem::u8path(file);
	}
	return std::filesystem::u8path(out_dir) / std::filesystem::u8path(file);
}

static void subtitle_log_write_meta(SourceContext *ctx)
{
	if (!ctx || !ctx->subtitle_log_stream.is_open()) {
		return;
	}

	obs_data_t *plugin_settings = ctx->source ? obs_source_get_settings(ctx->source) : nullptr;
	const std::string text_json = text_source_settings_json_filtered(ctx->internal_text_source);

	const char *src_name = ctx->source ? obs_source_get_name(ctx->source) : "";
	ctx->subtitle_log_stream << "[meta]\n";
	ctx->subtitle_log_stream << "source_name=" << (src_name ? src_name : "") << "\n";
	static const char *const kServerKeys[] = {
		"server_address", "server_port", "language_code", "model_id",
		"interim_results", "sample_rate_hertz", "audio_quantization",
		"chunk_duration_ms", "vad_threshold", "vad_min_silence_duration_ms",
		nullptr,
	};
	static const char *const kClientKeys[] = {
		"max_lines", "max_chars_per_line", "subtitle_log",
		nullptr,
	};
	ctx->subtitle_log_stream << "server_settings_json="
				<< obs_data_to_json_with_defaults_filtered_by_keys(plugin_settings, kServerKeys) << "\n";
	ctx->subtitle_log_stream << "client_settings_json="
				<< obs_data_to_json_with_defaults_filtered_by_keys(plugin_settings, kClientKeys) << "\n";
	ctx->subtitle_log_stream << "text_source_settings_json=" << text_json << "\n";
	ctx->subtitle_log_stream << "[/meta]\n";

	if (plugin_settings) {
		obs_data_release(plugin_settings);
	}
}

static void subtitle_log_close(SourceContext *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->subtitle_log_stream.is_open()) {
		ctx->subtitle_log_stream << "\n[end " << now_stamp_for_log() << "]\n";
		ctx->subtitle_log_stream.flush();
		ctx->subtitle_log_stream.close();
	}
	ctx->subtitle_log_last_final.clear();
}

static void subtitle_log_open_if_needed(SourceContext *ctx)
{
	if (!ctx || !ctx->subtitle_log_enabled || ctx->subtitle_log_stream.is_open() || !ctx->source) {
		return;
	}
	const std::filesystem::path path = build_subtitle_log_path(ctx->source);
	ctx->subtitle_log_stream.open(path, std::ios::out | std::ios::app | std::ios::binary);
	if (!ctx->subtitle_log_stream.is_open()) {
		obs_log(LOG_WARNING, "[sayo-obs-plugin] Failed to open subtitle log file");
		return;
	}
	subtitle_log_write_meta(ctx);

	ctx->subtitle_log_stream << "[start " << now_stamp_for_log() << "]\n";
	ctx->subtitle_log_stream.flush();
}

static size_t overlap_suffix_prefix(const std::string &a, const std::string &b)
{
	/* maximum k where suffix(a,k) == prefix(b,k) */
	const size_t max_k = std::min(a.size(), b.size());
	for (size_t k = max_k; k > 0; --k) {
		if (a.compare(a.size() - k, k, b, 0, k) == 0) {
			return k;
		}
	}
	return 0;
}

static void subtitle_log_append_interim(SourceContext *ctx, const std::string &text)
{
	if (!ctx || !ctx->subtitle_log_enabled || text.empty()) {
		return;
	}
	subtitle_log_open_if_needed(ctx);
	if (!ctx->subtitle_log_stream.is_open()) {
		return;
	}

	if (ctx->subtitle_log_last_final.empty()) {
		ctx->subtitle_log_stream << text;
		ctx->subtitle_log_stream.flush();
		ctx->subtitle_log_last_final = text;
		return;
	}

	if (text == ctx->subtitle_log_last_final) {
		return;
	}

	/* Most common: strict extension */
	if (text.rfind(ctx->subtitle_log_last_final, 0) == 0) {
		const std::string delta = text.substr(ctx->subtitle_log_last_final.size());
		if (!delta.empty()) {
			ctx->subtitle_log_stream << delta;
			ctx->subtitle_log_stream.flush();
		}
		ctx->subtitle_log_last_final = text;
		return;
	}

	/* Sliding window / partial rewrite: append only non-overlapping suffix */
	const size_t k = overlap_suffix_prefix(ctx->subtitle_log_last_final, text);
	if (k > 0 && k < text.size()) {
		const std::string delta = text.substr(k);
		if (!delta.empty()) {
			ctx->subtitle_log_stream << delta;
			ctx->subtitle_log_stream.flush();
		}
		ctx->subtitle_log_last_final = text;
		return;
	}

	/* Fallback: start a new line snapshot */
	ctx->subtitle_log_stream << "\n" << text;
	ctx->subtitle_log_stream.flush();
	ctx->subtitle_log_last_final = text;
}

static void hide_text_source_props(obs_properties_t *props)
{
	if (!props) {
		return;
	}
	static const char *const kHide[] = {
		/* Text input mode (manual/file) */
		"from_file",
		"text_file",
		/* Chat log options */
		"log_mode",
		"log_lines",
		/* Layout options we control ourselves */
		"custom_width",
		"word_wrap",
		nullptr,
	};
	for (const char *const *p = kHide; *p; ++p) {
		obs_properties_remove_by_name(props, *p);
	}
}

static void normalize_text_style(obs_data_t *settings)
{
	if (!settings) {
		return;
	}
	if (!obs_data_has_user_value(settings, "antialiasing")) {
		obs_data_set_bool(settings, "antialiasing", true);
	}
}

static std::string get_display_text_locked(const SourceContext *ctx)
{
	if (!ctx->asr_received_content) {
		return ctx->subtitles_buffer.formatAsFinalPreview(ctx->cached_user_text);
	}
	return ctx->subtitles_buffer.getBufferContent();
}

static void apply_text_source_style_from_parent(SourceContext *ctx, obs_data_t *parent_settings);
static void apply_text_source_text(SourceContext *ctx, const std::string &display_text);
static void queue_text_refresh(SourceContext *ctx);

static void apply_preview_from_settings(SourceContext *ctx, obs_data_t *settings)
{
	if (!ctx || !settings) {
		return;
	}

	std::string display_body;
	{
		std::lock_guard<std::mutex> lock(ctx->subtitle_mutex);
		ctx->cached_user_text = obs_data_get_string(settings, "text");
		const int max_lines = static_cast<int>(obs_data_get_int(settings, "max_lines"));
		const int max_chars = static_cast<int>(obs_data_get_int(settings, "max_chars_per_line"));
		ctx->subtitles_buffer.changeSize(static_cast<size_t>(max_lines), static_cast<size_t>(max_chars));
		display_body = get_display_text_locked(ctx);
		ctx->pending_display_text = display_body;
	}

	apply_text_source_style_from_parent(ctx, settings);
	apply_text_source_text(ctx, display_body);
	queue_text_refresh(ctx);
}

static void apply_text_source_style_from_parent(SourceContext *ctx, obs_data_t *parent_settings)
{
	if (!ctx || !ctx->internal_text_source || !parent_settings) {
		return;
	}
	obs_data_t *tmp = obs_data_create();
	obs_data_apply(tmp, parent_settings);
	obs_data_set_bool(tmp, "from_file", false);
	obs_data_set_bool(tmp, "word_wrap", false);
	obs_data_set_int(tmp, "custom_width", 0);
	obs_source_update(ctx->internal_text_source, tmp);
	obs_data_release(tmp);
}

static void apply_text_source_text(SourceContext *ctx, const std::string &display_text)
{
	if (!ctx || !ctx->internal_text_source) {
		return;
	}

	/* Update text only, keep style/layout intact. */
	obs_data_t *s = obs_source_get_settings(ctx->internal_text_source);
	if (!s) {
		return;
	}
	obs_data_set_string(s, "text", display_text.c_str());
	obs_source_update(ctx->internal_text_source, s);
	obs_data_release(s);
}

static void ensure_model_and_language_selected(SourceContext *ctx);

struct TextRefreshArgs {
	obs_weak_source_t *weak_source;
};

static void apply_text_refresh_ui(void *param)
{
	auto *args = static_cast<TextRefreshArgs *>(param);
	if (!args || !args->weak_source) {
		delete args;
		return;
	}

	obs_source_t *src = obs_weak_source_get_source(args->weak_source);
	obs_weak_source_release(args->weak_source);
	delete args;
	if (!src) {
		return;
	}

	auto *ctx = static_cast<SourceContext *>(obs_obj_get_data(src));
	if (!ctx || ctx->shutting_down || !ctx->internal_text_source) {
		obs_source_release(src);
		return;
	}

	std::string body;
	{
		std::lock_guard<std::mutex> lock(ctx->subtitle_mutex);
		body = ctx->pending_display_text;
	}
	apply_text_source_text(ctx, body);
	{
		std::lock_guard<std::mutex> lock(ctx->subtitle_mutex);
		ctx->last_applied_text = body;
	}

	obs_source_release(src);
}

static void queue_text_refresh(SourceContext *ctx)
{
	if (!ctx || ctx->shutting_down || !ctx->source) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(ctx->subtitle_mutex);
		if (ctx->pending_display_text == ctx->last_applied_text) {
			return;
		}
	}

	auto *args = new TextRefreshArgs{};
	args->weak_source = obs_source_get_weak_source(ctx->source);
	if (!args->weak_source) {
		delete args;
		return;
	}
	obs_queue_task(OBS_TASK_UI, apply_text_refresh_ui, args, false);
}

static bool is_stereo(const audio_data *audio)
{
	return audio && audio->data[0] && audio->data[1];
}

static std::vector<char> quantize_to_s16(const float *data, size_t frame_count)
{
	std::vector<char> out(frame_count * sizeof(int16_t));
	auto *dst = reinterpret_cast<int16_t *>(out.data());
	for (size_t i = 0; i < frame_count; ++i) {
		const float clamped = std::max(-1.0f, std::min(1.0f, data[i]));
		dst[i] = static_cast<int16_t>(clamped * 32767.0f);
	}
	return out;
}

static size_t resample_audio(SourceContext *ctx, const float *input, size_t in_frames)
{
	if (!ctx || !ctx->resampler) {
		return 0;
	}
	const size_t max_out_frames = static_cast<size_t>(static_cast<float>(in_frames) * ctx->resample_ratio) + 2;
	ctx->resample_output_buffer.resize(max_out_frames);

	SRC_DATA data{};
	data.data_in = input;
	data.input_frames = static_cast<long>(in_frames);
	data.data_out = ctx->resample_output_buffer.data();
	data.output_frames = static_cast<long>(max_out_frames);
	data.src_ratio = ctx->resample_ratio;
	data.end_of_input = 0;

	const int err = src_process(ctx->resampler, &data);
	if (err != 0) {
		obs_log(LOG_ERROR, "Resample failed: %s", src_strerror(err));
		return 0;
	}
	return static_cast<size_t>(data.output_frames_gen);
}

static void rebuild_audio_pipeline(SourceContext *ctx)
{
	if (!ctx) {
		return;
	}
	ctx->resample_ratio = static_cast<float>(ctx->settings.sample_rate_hertz) /
	                      static_cast<float>(ctx->input_sample_rate);
	const size_t samples_per_chunk = static_cast<size_t>(ctx->settings.sample_rate_hertz) *
	                                 static_cast<size_t>(ctx->settings.chunk_duration_ms) / 1000;
	const size_t sample_size = ctx->settings.audio_quantization == sayo::AUDIO_QUANTIZATION_PCM_S16LE
	                               ? sizeof(int16_t)
	                               : sizeof(float);
	ctx->audio_chunk_size_bytes = std::max<size_t>(sample_size, samples_per_chunk * sample_size);
	ctx->send_buffer.clear();
	ctx->resampler_warmup = kResamplerWarmupFrames;
}

static void audio_callback(void *param, obs_source_t *, const audio_data *audio, bool muted)
{
	auto *ctx = static_cast<SourceContext *>(param);
	if (!ctx || muted || !audio || !audio->data[0]) {
		return;
	}

	std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
	if (!ctx->grpc_client || !ctx->grpc_client->IsRunning()) {
		return;
	}

	const size_t frames = audio->frames;
	std::vector<float> mono(frames);
	const auto *left = reinterpret_cast<const float *>(audio->data[0]);
	if (is_stereo(audio)) {
		const auto *right = reinterpret_cast<const float *>(audio->data[1]);
		for (size_t i = 0; i < frames; ++i) {
			mono[i] = (left[i] + right[i]) * 0.5f;
		}
	} else {
		std::memcpy(mono.data(), left, frames * sizeof(float));
	}

	const size_t out_frames = resample_audio(ctx, mono.data(), frames);
	if (out_frames == 0) {
		return;
	}

	if (ctx->resampler_warmup > 0) {
		--ctx->resampler_warmup;
		return;
	}

	std::vector<char> encoded;
	if (ctx->settings.audio_quantization == sayo::AUDIO_QUANTIZATION_PCM_S16LE) {
		encoded = quantize_to_s16(ctx->resample_output_buffer.data(), out_frames);
	} else {
		encoded.resize(out_frames * sizeof(float));
		std::memcpy(encoded.data(), ctx->resample_output_buffer.data(), encoded.size());
	}

	ctx->send_buffer.insert(ctx->send_buffer.end(), encoded.begin(), encoded.end());
	while (ctx->send_buffer.size() >= ctx->audio_chunk_size_bytes) {
		std::vector<char> chunk(ctx->send_buffer.begin(), ctx->send_buffer.begin() + ctx->audio_chunk_size_bytes);
		ctx->send_buffer.erase(ctx->send_buffer.begin(), ctx->send_buffer.begin() + ctx->audio_chunk_size_bytes);
		ctx->grpc_client->SendAudioChunk(std::move(chunk));
	}
}

static void asr_update(void *data, obs_data_t *settings)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (!ctx) {
		return;
	}

	/* Apply settings snapshot (OK/Apply) into runtime fields */
	const bool prev_log_enabled = ctx->subtitle_log_enabled;
	ctx->subtitle_log_enabled = obs_data_get_bool(settings, "subtitle_log");

	ctx->settings.server_address = obs_data_get_string(settings, "server_address");
	ctx->settings.server_port = static_cast<int>(obs_data_get_int(settings, "server_port"));
	ctx->settings.model_id = obs_data_get_string(settings, "model_id");
	ctx->settings.language_code = obs_data_get_string(settings, "language_code");
	ctx->settings.interim_results = obs_data_get_bool(settings, "interim_results");
	ctx->settings.sample_rate_hertz = static_cast<int>(obs_data_get_int(settings, "sample_rate_hertz"));
	ctx->settings.audio_quantization = static_cast<sayo::AudioQuantization>(
		static_cast<int>(obs_data_get_int(settings, "audio_quantization")));
	ctx->settings.chunk_duration_ms = static_cast<int>(obs_data_get_int(settings, "chunk_duration_ms"));
	ctx->settings.vad_threshold = static_cast<float>(obs_data_get_double(settings, "vad_threshold"));
	ctx->settings.vad_min_silence_duration_ms =
		static_cast<int>(obs_data_get_int(settings, "vad_min_silence_duration_ms"));
	ctx->settings.max_lines = static_cast<int>(obs_data_get_int(settings, "max_lines"));
	ctx->settings.max_chars_per_line = static_cast<int>(obs_data_get_int(settings, "max_chars_per_line"));

	const std::string selected_audio = obs_data_get_string(settings, "audio_source");
	if (ctx->settings.audio_source_name != selected_audio) {
		if (!ctx->settings.audio_source_name.empty()) {
			if (obs_source_t *old_src = obs_get_source_by_name(ctx->settings.audio_source_name.c_str())) {
				obs_source_remove_audio_capture_callback(old_src, audio_callback, ctx);
				obs_source_release(old_src);
			}
		}
		if (!selected_audio.empty()) {
			if (obs_source_t *new_src = obs_get_source_by_name(selected_audio.c_str())) {
				obs_source_add_audio_capture_callback(new_src, audio_callback, ctx);
				obs_source_release(new_src);
			}
		}
		ctx->settings.audio_source_name = selected_audio;
	}

	apply_preview_from_settings(ctx, settings);
	rebuild_audio_pipeline(ctx);

	if (ctx->connect_pending_apply.exchange(false)) {
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_client) {
			ctx->grpc_client->Stop();
		}
		ensure_model_and_language_selected(ctx);
		ctx->grpc_client = std::make_unique<ASRGrpcClient>(ctx->settings.server_address, ctx->settings.server_port);
		const bool started = ctx->grpc_client->Start(sayo_plugin::ToStreamingConfig(ctx->settings));
		if (!started) {
			ctx->streaming_active = false;
			ctx->grpc_handshake_pending = false;
			ctx->connect_status = "Stream start failed";
		} else {
			ctx->streaming_active = false;
			ctx->grpc_handshake_pending = true;
			ctx->connect_status = "Waiting for server…";
		}

		/* Rotate subtitle log on new connection; file opens only after session is connected. */
		if (started) {
			subtitle_log_close(ctx);
		}
	}

	/* Toggle logging without rotating file mid-session */
	if (prev_log_enabled && !ctx->subtitle_log_enabled) {
		subtitle_log_close(ctx);
	} else if (!prev_log_enabled && ctx->subtitle_log_enabled) {
		if (ctx->streaming_active.load()) {
			subtitle_log_open_if_needed(ctx);
		}
	}
}

static std::string humanize_connection_status(const std::string &code)
{
	if (code == "config_accepted")
		return "Verifying configuration…";
	if (code == "allocating_session")
		return "Allocating session… (may take tens of seconds)";
	if (code == "actor_reserved")
		return "Reserving worker…";
	if (code == "session_opening")
		return "Connecting to model…";
	if (code == "connected")
		return "Connected";
	if (code == "error")
		return "Error";
	if (code.empty())
		return {};
	return code;
}

static bool is_connected(const SourceContext *ctx)
{
	return ctx && ctx->streaming_active.load();
}

static void ensure_model_and_language_selected(SourceContext *ctx)
{
	if (!ctx) {
		return;
	}
	if (ctx->available_models.empty()) {
		return;
	}

	if (ctx->settings.model_id.empty()) {
		ctx->settings.model_id = ctx->available_models.front().model_id();
	}
	if (ctx->settings.language_code.empty()) {
		for (const auto &m : ctx->available_models) {
			if (m.model_id() == ctx->settings.model_id) {
				ctx->settings.language_code = m.language_code();
				break;
			}
		}
		if (ctx->settings.language_code.empty()) {
			ctx->settings.language_code = ctx->available_models.front().language_code();
		}
	}
}

struct ConnectResultArgs {
	obs_weak_source_t *weak_source;
	bool success;
	std::string status;
	std::vector<sayo::ModelDescriptor> models;
};

static void on_connect_ui_update(void *param)
{
	auto *args = static_cast<ConnectResultArgs *>(param);
	if (!args || !args->weak_source) {
		delete args;
		return;
	}
	obs_source_t *src = obs_weak_source_get_source(args->weak_source);
	obs_weak_source_release(args->weak_source);
	args->weak_source = nullptr;
	if (!src) {
		delete args;
		return;
	}
	auto *ctx = static_cast<SourceContext *>(obs_obj_get_data(src));
	if (!ctx || ctx->shutting_down) {
		obs_source_release(src);
		delete args;
		return;
	}
	ctx->connect_status = args->status;
	ctx->connect_in_progress = false;
	if (args->success) {
		ctx->available_models = std::move(args->models);
		/* User will pick model and confirm with OK/Apply. */
	}
	queue_text_refresh(ctx);
	obs_source_update_properties(src);
	obs_source_release(src);
	delete args;
}

struct ConnectionUiArgs {
	obs_weak_source_t *weak_source = nullptr;
	enum class Kind { Progress, Connected, StreamEnded, ServerError } kind = Kind::Progress;
	std::string status_text;
};

static void on_connection_ui(void *param)
{
	auto *args = static_cast<ConnectionUiArgs *>(param);
	if (!args || !args->weak_source) {
		delete args;
		return;
	}
	obs_source_t *src = obs_weak_source_get_source(args->weak_source);
	obs_weak_source_release(args->weak_source);
	args->weak_source = nullptr;
	if (!src) {
		delete args;
		return;
	}
	auto *ctx = static_cast<SourceContext *>(obs_obj_get_data(src));
	if (!ctx || ctx->shutting_down) {
		obs_source_release(src);
		delete args;
		return;
	}

	switch (args->kind) {
	case ConnectionUiArgs::Kind::Progress:
		ctx->connect_status = std::move(args->status_text);
		break;
	case ConnectionUiArgs::Kind::Connected:
		ctx->grpc_handshake_pending = false;
		ctx->streaming_active = true;
		ctx->connect_status = "Connected";
		if (ctx->subtitle_log_enabled) {
			subtitle_log_open_if_needed(ctx);
		}
		break;
	case ConnectionUiArgs::Kind::StreamEnded:
	case ConnectionUiArgs::Kind::ServerError: {
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_client) {
			ctx->grpc_client->Stop();
			if (args->kind == ConnectionUiArgs::Kind::ServerError) {
				ctx->grpc_client.reset();
			}
		}
	}
		ctx->grpc_handshake_pending = false;
		ctx->streaming_active = false;
		ctx->connect_status = std::move(args->status_text);
		break;
	}

	queue_text_refresh(ctx);
	obs_source_update_properties(src);
	obs_source_release(src);
	delete args;
}

static void queue_connection_ui(SourceContext *ctx, ConnectionUiArgs *args)
{
	if (!ctx || !args || ctx->shutting_down || !ctx->source) {
		delete args;
		return;
	}
	args->weak_source = obs_source_get_weak_source(ctx->source);
	if (!args->weak_source) {
		delete args;
		return;
	}
	obs_queue_task(OBS_TASK_UI, on_connection_ui, args, false);
}

static bool on_disconnect_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (!ctx || ctx->shutting_down) {
		return true;
	}
	{
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_client) {
			ctx->grpc_client->Stop();
			ctx->grpc_client.reset();
		}
	}
	ctx->streaming_active = false;
	ctx->grpc_handshake_pending = false;
	subtitle_log_close(ctx);
	ctx->connect_pending_apply = false;
	{
		std::lock_guard<std::mutex> lock(ctx->subtitle_mutex);
		ctx->settings.model_id.clear();
		ctx->settings.language_code.clear();
	}

	ctx->available_models.clear();
	ctx->connect_status = "Disconnected (HealthCheck required)";
	return true;
}

static bool on_connect_clicked(obs_properties_t *, obs_property_t *, void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (!ctx || ctx->shutting_down || ctx->connect_in_progress.exchange(true)) {
		return true;
	}

	if (ctx->connect_thread.joinable()) {
		ctx->connect_thread.join();
	}

	ctx->connect_status = "Connecting";
	ctx->connect_thread = std::thread([ctx]() {
		auto *args = new ConnectResultArgs{};
		args->weak_source = ctx->source ? obs_source_get_weak_source(ctx->source) : nullptr;
		args->success = false;
		args->status = "Failed";
		if (!args->weak_source) {
			if (!ctx->shutting_down) {
				ctx->connect_in_progress = false;
			}
			delete args;
			return;
		}

		/* Use current UI snapshot (even if settings are not applied yet). */
		obs_data_t *ui_settings = ctx->source ? obs_source_get_settings(ctx->source) : nullptr;
		const std::string server =
			ui_settings ? obs_data_get_string(ui_settings, "server_address") : ctx->settings.server_address;
		const int port = ui_settings ? static_cast<int>(obs_data_get_int(ui_settings, "server_port"))
					     : ctx->settings.server_port;
		if (ui_settings) {
			obs_data_release(ui_settings);
		}

		auto client = std::make_unique<ASRGrpcClient>(server, port);
		sayo::HealthCheckResponse response;
		std::string error;
		if (!client->HealthCheck(response, error)) {
			args->status = error.empty() ? "Failed" : error;
			if (!ctx->shutting_down) {
				obs_queue_task(OBS_TASK_UI, on_connect_ui_update, args, true);
			} else {
				obs_weak_source_release(args->weak_source);
				ctx->connect_in_progress = false;
				delete args;
			}
			return;
		}
		args->success = true;
		args->status = "HealthCheck OK (press OK to connect)";
		args->models.assign(response.models().begin(), response.models().end());
		ctx->connect_pending_apply = true;
		if (!ctx->shutting_down) {
			obs_queue_task(OBS_TASK_UI, on_connect_ui_update, args, true);
		} else {
			obs_weak_source_release(args->weak_source);
			ctx->connect_in_progress = false;
			delete args;
		}
	});
	return true;
}

static obs_properties_t *asr_get_properties(void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	obs_properties_t *props = obs_properties_create();
	obs_properties_set_flags(props, OBS_PROPERTIES_DEFER_UPDATE);

	obs_property_t *audio_list = obs_properties_add_list(
		props, "audio_source", "Audio source", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_enum_sources(
		[](void *p, obs_source_t *src) {
			if (obs_source_get_output_flags(src) & OBS_SOURCE_AUDIO) {
				const char *name = obs_source_get_name(src);
				obs_property_list_add_string(static_cast<obs_property_t *>(p), name, name);
			}
			return true;
		},
		audio_list);

	auto preview_cb = [](void *priv, obs_properties_t *, obs_property_t *, obs_data_t *settings) -> bool {
		auto *ctx2 = static_cast<SourceContext *>(priv);
		if (ctx2 && settings) {
			apply_preview_from_settings(ctx2, settings);
		}
		return false;
	};

	obs_properties_add_text(props, "server_address", "Server address", OBS_TEXT_DEFAULT);
	obs_properties_add_int(props, "server_port", "Server port", 1, 65535, 1);
	obs_properties_add_button(props, "connect_button", "HealthCheck", on_connect_clicked);
	obs_properties_add_button(props, "disconnect_button", "Disconnect", on_disconnect_clicked);
	const std::string connection_status = "Connection: " + ctx->connect_status;
	add_info_text(props, "connection_status", connection_status.c_str(), true);

	obs_property_t *model_list = obs_properties_add_list(
		props, "model_id", "Model", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	for (const auto &model : ctx->available_models) {
		const std::string cap = sayo_plugin::ModelCaption(model);
		obs_property_list_add_string(model_list, cap.c_str(), model.model_id().c_str());
	}

	obs_property_t *lang_list = obs_properties_add_list(
		props, "language_code", "Language", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	{
		obs_data_t *cur = ctx->source ? obs_source_get_settings(ctx->source) : nullptr;
		const std::string sel_model = cur ? obs_data_get_string(cur, "model_id") : ctx->settings.model_id;
		if (cur) {
			obs_data_release(cur);
		}
		for (const auto &m : ctx->available_models) {
			if (!sel_model.empty() && m.model_id() != sel_model) {
				continue;
			}
			const std::string cap = m.language_code();
			obs_property_list_add_string(lang_list, cap.c_str(), m.language_code().c_str());
		}
	}
	obs_properties_add_bool(props, "interim_results", "Interim results");
	obs_property_t *subtitle_log = obs_properties_add_bool(props, "subtitle_log", "Subtitle log");
	obs_properties_add_int(props, "sample_rate_hertz", "Sample rate", 8000, 96000, 1);

	obs_property_t *aq = obs_properties_add_list(
		props, "audio_quantization", "Audio quantization", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(aq, "PCM_F32LE", sayo::AUDIO_QUANTIZATION_PCM_F32LE);
	obs_property_list_add_int(aq, "PCM_S16LE", sayo::AUDIO_QUANTIZATION_PCM_S16LE);

	obs_properties_add_int(props, "chunk_duration_ms", "Chunk duration (ms)", 20, 1000, 1);
	obs_properties_add_float_slider(props, "vad_threshold", "VAD threshold", 0.01, 1.0, 0.01);
	obs_properties_add_int(props, "vad_min_silence_duration_ms", "VAD min silence (ms)", 50, 5000, 10);
	obs_property_t *max_lines = obs_properties_add_int(props, "max_lines", "Max lines", 1, 10, 1);
	obs_property_t *max_chars = obs_properties_add_int(props, "max_chars_per_line", "Max chars per line", 10, 200, 1);

	/* These are preview-only: apply immediately while editing */
	obs_property_t *text_prop = obs_properties_get(props, "text");
	if (text_prop)
		obs_property_set_modified_callback2(text_prop, preview_cb, ctx);
	if (max_lines)
		obs_property_set_modified_callback2(max_lines, preview_cb, ctx);
	if (max_chars)
		obs_property_set_modified_callback2(max_chars, preview_cb, ctx);
	if (subtitle_log)
		obs_property_set_modified_callback2(subtitle_log, preview_cb, ctx);

	add_info_text(
		props, "subtitle_appearance_note",
		"Text acts as a placeholder until the first non-empty ASR result. After that, incoming ASR text is used.");

	if (ctx->internal_text_source) {
		obs_properties_t *text_props = obs_source_properties(ctx->internal_text_source);
		if (text_props) {
			hide_text_source_props(text_props);
			/* Make text appearance changes apply immediately (preview). */
			for (obs_property_t *p = obs_properties_first(text_props); p; obs_property_next(&p)) {
				obs_property_set_modified_callback2(p, preview_cb, ctx);
			}
			obs_properties_add_group(props, "subtitle_text_ft2", "Subtitle text (FreeType)", OBS_GROUP_NORMAL,
						 text_props);
		}
	}

	/* Freeze server-related settings when connected */
	const bool connected = is_connected(ctx);
	if (obs_property_t *p = obs_properties_get(props, "server_address"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "server_port"))
		obs_property_set_enabled(p, !connected);
	if (model_list)
		obs_property_set_enabled(model_list, !connected);
	if (lang_list)
		obs_property_set_enabled(lang_list, !connected);
	if (obs_property_t *p = obs_properties_get(props, "interim_results"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "sample_rate_hertz"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "audio_quantization"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "chunk_duration_ms"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "vad_threshold"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "vad_min_silence_duration_ms"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "connect_button"))
		obs_property_set_enabled(p, !connected);
	if (obs_property_t *p = obs_properties_get(props, "disconnect_button"))
		obs_property_set_enabled(p, connected);

	return props;
}

static void asr_tick(void *data, float)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (!ctx) {
		return;
	}

	std::vector<RecognitionResult> batch;
	{
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_client) {
			std::lock_guard<std::mutex> queue_lock(ctx->grpc_client->queue_mutex);
			while (!ctx->grpc_client->results_queue.empty()) {
				batch.push_back(std::move(ctx->grpc_client->results_queue.front()));
				ctx->grpc_client->results_queue.pop();
			}
		}
	}

	for (RecognitionResult &result : batch) {
		if (!result.connection_status.empty()) {
			const std::string &cs = result.connection_status;
			if (cs == "connected") {
				auto *args = new ConnectionUiArgs{};
				args->kind = ConnectionUiArgs::Kind::Connected;
				queue_connection_ui(ctx, args);
			} else if (cs == "error") {
				ctx->grpc_handshake_pending = false;
				std::string msg = humanize_connection_status(cs);
				if (!result.connection_detail.empty()) {
					msg += ": ";
					msg += result.connection_detail;
				}
				auto *args = new ConnectionUiArgs{};
				args->kind = ConnectionUiArgs::Kind::ServerError;
				args->status_text = std::move(msg);
				queue_connection_ui(ctx, args);
			} else {
				auto *args = new ConnectionUiArgs{};
				args->kind = ConnectionUiArgs::Kind::Progress;
				args->status_text = humanize_connection_status(cs);
				if (args->status_text.empty()) {
					args->status_text = cs;
				}
				queue_connection_ui(ctx, args);
			}
		}

		if (!result.transcript.empty()) {
			{
				std::lock_guard<std::mutex> sub(ctx->subtitle_mutex);
				ctx->subtitles_buffer.addText(result.transcript, result.is_final);
				ctx->asr_received_content = true;
				ctx->pending_display_text = get_display_text_locked(ctx);
			}
			subtitle_log_append_interim(ctx, result.transcript);
			queue_text_refresh(ctx);
		}
	}

	bool need_stream_lost = false;
	{
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_handshake_pending.load() && ctx->grpc_client && !ctx->grpc_client->IsRunning() &&
		    !ctx->streaming_active.load()) {
			ctx->grpc_handshake_pending = false;
			need_stream_lost = true;
		}
	}
	if (need_stream_lost) {
		auto *args = new ConnectionUiArgs{};
		args->kind = ConnectionUiArgs::Kind::StreamEnded;
		args->status_text = "Connection closed before session was ready";
		queue_connection_ui(ctx, args);
	}
}

static void asr_render(void *data, gs_effect_t *)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (ctx && ctx->internal_text_source) {
		obs_source_video_render(ctx->internal_text_source);
	}
}

static uint32_t asr_get_width(void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	return (ctx && ctx->internal_text_source) ? obs_source_get_width(ctx->internal_text_source) : 0;
}

static uint32_t asr_get_height(void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	return (ctx && ctx->internal_text_source) ? obs_source_get_height(ctx->internal_text_source) : 0;
}

static void asr_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "server_address", "localhost");
	obs_data_set_default_int(settings, "server_port", 50051);
	obs_data_set_default_bool(settings, "interim_results", true);
	obs_data_set_default_int(settings, "sample_rate_hertz", 16000);
	obs_data_set_default_int(settings, "audio_quantization", sayo::AUDIO_QUANTIZATION_PCM_F32LE);
	obs_data_set_default_int(settings, "chunk_duration_ms", 100);
	obs_data_set_default_double(settings, "vad_threshold", 0.5);
	obs_data_set_default_int(settings, "vad_min_silence_duration_ms", 500);
	obs_data_set_default_int(settings, "max_lines", 2);
	obs_data_set_default_int(settings, "max_chars_per_line", 60);
	obs_data_set_default_bool(settings, "subtitle_log", false);
	obs_data_set_default_bool(settings, "antialiasing", true);
	obs_data_set_default_int(settings, "color1", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "color2", 0xFFFFFFFF);

	if (obs_data_t *text_defaults = obs_get_source_defaults("text_ft2_source")) {
		obs_data_apply(settings, text_defaults);
		obs_data_release(text_defaults);
	}
}

static void *asr_create(obs_data_t *settings, obs_source_t *source)
{
	auto *ctx = new SourceContext;
	ctx->source = source;

	obs_data_t *text_settings = obs_data_create();
	obs_data_set_string(text_settings, "text", "");

	std::string name;
	obs_source_t *exists = nullptr;
	do {
		name = "__sayo_internal_text_" + std::to_string(asr_instance_counter++);
		if (exists) {
			obs_source_release(exists);
		}
		exists = obs_get_source_by_name(name.c_str());
	} while (exists);

	ctx->internal_text_source = obs_source_create("text_ft2_source", name.c_str(), text_settings, nullptr);
	obs_data_release(text_settings);

	if (const audio_output_info *info = audio_output_get_info(obs_get_audio())) {
		ctx->input_sample_rate = info->samples_per_sec;
	}
	int err = 0;
	ctx->resampler = src_new(SRC_SINC_FASTEST, 1, &err);
	if (!ctx->resampler) {
		obs_log(LOG_ERROR, "Failed to create resampler: %s", src_strerror(err));
	}
	asr_update(ctx, settings);
	return ctx;
}

static void asr_destroy(void *data)
{
	auto *ctx = static_cast<SourceContext *>(data);
	if (!ctx) {
		return;
	}
	subtitle_log_close(ctx);
	ctx->shutting_down = true;
	if (ctx->connect_thread.joinable()) {
		ctx->connect_thread.join();
	}

	if (!ctx->settings.audio_source_name.empty()) {
		if (obs_source_t *audio_src = obs_get_source_by_name(ctx->settings.audio_source_name.c_str())) {
			obs_source_remove_audio_capture_callback(audio_src, audio_callback, ctx);
			obs_source_release(audio_src);
		}
	}

	{
		std::lock_guard<std::mutex> lock(ctx->grpc_mutex);
		if (ctx->grpc_client) {
			ctx->grpc_client->Stop();
			ctx->grpc_client.reset();
		}
	}

	if (ctx->internal_text_source) {
		obs_source_release(ctx->internal_text_source);
	}
	if (ctx->resampler) {
		src_delete(ctx->resampler);
	}
	delete ctx;
}

/* C++17 / MSVC: positional aggregate init (not .field = … designated init, which needs /std:c++20 here). */
static obs_source_info asr_source_info = {
	"sayo_asr_text_source",
	OBS_SOURCE_TYPE_INPUT,
	OBS_SOURCE_VIDEO,
	asr_get_name,
	asr_create,
	asr_destroy,
	asr_get_width,
	asr_get_height,
	asr_get_defaults,
	asr_get_properties,
	asr_update,
	nullptr,
	nullptr,
	nullptr,
	nullptr,
	asr_tick,
	asr_render,
};

extern "C" {
const char *obs_module_name(void)
{
	return "Sayo OBS ASR Plugin";
}

const char *obs_module_description(void)
{
	return "Transcribes selected OBS audio source over gRPC streaming API.";
}

bool obs_module_load(void)
{
	obs_register_source(&asr_source_info);
	obs_log(LOG_INFO, "plugin loaded (version %s)", PLUGIN_VERSION);
	return true;
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
}
