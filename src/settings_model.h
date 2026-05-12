#pragma once

#include "sayo.pb.h"

#include <string>
#include <vector>

namespace sayo_plugin {

struct RuntimeSettings {
	std::string server_address = "localhost";
	int server_port = 50051;

	std::string model_id;
	std::string language_code = "ru-RU";
	bool interim_results = true;
	int sample_rate_hertz = 16000;
	sayo::AudioQuantization audio_quantization = sayo::AUDIO_QUANTIZATION_PCM_F32LE;
	int chunk_duration_ms = 100;
	float vad_threshold = 0.5f;
	int vad_min_silence_duration_ms = 500;

	int max_lines = 2;
	int max_chars_per_line = 60;
	std::string audio_source_name;
};

inline sayo::StreamingConfig ToStreamingConfig(const RuntimeSettings &settings)
{
	sayo::StreamingConfig cfg;
	cfg.set_model_id(settings.model_id);
	cfg.set_language_code(settings.language_code);
	cfg.set_interim_results(settings.interim_results);
	cfg.set_sample_rate_hertz(settings.sample_rate_hertz);
	cfg.set_audio_quantization(settings.audio_quantization);
	cfg.set_chunk_duration_ms(settings.chunk_duration_ms);
	cfg.set_vad_threshold(settings.vad_threshold);
	cfg.set_vad_min_silence_duration_ms(settings.vad_min_silence_duration_ms);
	return cfg;
}

inline std::string ModelCaption(const sayo::ModelDescriptor &model)
{
	return model.model_id() + " (" + model.language_code() + ", " +
	       std::to_string(model.sample_rate_hertz()) + "Hz)";
}

} // namespace sayo_plugin
