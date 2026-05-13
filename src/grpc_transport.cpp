#include "grpc_transport.h"

#include <obs-module.h>
#include <plugin-support.h>

ASRGrpcClient::ASRGrpcClient(const std::string &server, int port)
{
	const std::string address = server + ":" + std::to_string(port);
	channel_ = grpc::CreateChannel(address, grpc::InsecureChannelCredentials());
	stub_ = sayo::SayoService::NewStub(channel_);
}

ASRGrpcClient::~ASRGrpcClient()
{
	Stop();
}

bool ASRGrpcClient::HealthCheck(sayo::HealthCheckResponse &response, std::string &error_message)
{
	grpc::ClientContext health_context;
	health_context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
	sayo::HealthCheckRequest request;
	const grpc::Status status = stub_->HealthCheck(&health_context, request, &response);
	if (!status.ok()) {
		error_message = status.error_message();
		return false;
	}
	if (!response.ready()) {
		error_message = response.message();
		return false;
	}
	return true;
}

bool ASRGrpcClient::Start(const sayo::StreamingConfig &config)
{
	if (running_) {
		return true;
	}

	active_config_ = config;
	context_ = std::make_unique<grpc::ClientContext>();
	stream_ = stub_->StreamingRecognize(context_.get());
	if (!stream_) {
		obs_log(LOG_ERROR, "Failed to create StreamingRecognize stream");
		return false;
	}

	sayo::StreamingRecognizeRequest config_message;
	*config_message.mutable_config() = active_config_;
	if (!stream_->Write(config_message)) {
		obs_log(LOG_ERROR, "Failed to send initial StreamingConfig");
		stream_.reset();
		context_.reset();
		return false;
	}

	running_ = true;
	sender_thread_ = std::thread(&ASRGrpcClient::SenderLoop, this);
	receiver_thread_ = std::thread(&ASRGrpcClient::ReceiverLoop, this);
	return true;
}

void ASRGrpcClient::Stop()
{
	if (!running_ && !stream_) {
		return;
	}
	running_ = false;
	audio_queue_cv_.notify_all();

	if (context_) {
		context_->TryCancel();
	}
	if (stream_) {
		stream_->WritesDone();
	}
	if (sender_thread_.joinable()) {
		sender_thread_.join();
	}
	if (receiver_thread_.joinable()) {
		receiver_thread_.join();
	}
	if (stream_) {
		const grpc::Status status = stream_->Finish();
		if (!status.ok()) {
			if (status.error_code() == grpc::StatusCode::CANCELLED) {
				obs_log(LOG_INFO, "StreamingRecognize finished: %s", status.error_message().c_str());
			} else {
				obs_log(LOG_ERROR, "StreamingRecognize finished with error: %s", status.error_message().c_str());
			}
		}
	}

	{
		std::lock_guard<std::mutex> lock(audio_queue_mutex_);
		std::queue<std::vector<char>> empty;
		audio_queue_.swap(empty);
	}

	stream_.reset();
	context_.reset();
}

void ASRGrpcClient::SendAudioChunk(std::vector<char> chunk)
{
	if (!running_) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(audio_queue_mutex_);
		audio_queue_.push(std::move(chunk));
	}
	audio_queue_cv_.notify_one();
}

bool ASRGrpcClient::IsRunning() const
{
	return running_;
}

void ASRGrpcClient::SenderLoop()
{
	while (running_) {
		std::vector<char> chunk;
		{
			std::unique_lock<std::mutex> lock(audio_queue_mutex_);
			audio_queue_cv_.wait(lock, [&]() { return !running_ || !audio_queue_.empty(); });
			if (!running_) {
				break;
			}
			chunk = std::move(audio_queue_.front());
			audio_queue_.pop();
		}

		sayo::StreamingRecognizeRequest message;
		message.set_audio_chunk(chunk.data(), static_cast<int>(chunk.size()));
		if (!stream_->Write(message)) {
			obs_log(LOG_ERROR, "Failed to write audio chunk into stream");
			running_ = false;
			break;
		}
	}
}

void ASRGrpcClient::ReceiverLoop()
{
	sayo::StreamingRecognizeResponse response;
	while (running_ && stream_) {
		if (!stream_->Read(&response)) {
			break;
		}

		RecognitionResult item;
		item.transcript = response.transcript();
		item.is_final = response.is_final();
		item.confidence = response.confidence();

		const auto &meta = response.metadata();
		if (const auto it = meta.find("connection_status"); it != meta.end()) {
			item.connection_status = it->second;
		}
		if (const auto it = meta.find("detail"); it != meta.end()) {
			item.connection_detail = it->second;
		}

		if (!item.connection_status.empty()) {
			std::string dbg = "connection_status=" + item.connection_status;
			if (const auto a = meta.find("actor_name"); a != meta.end()) {
				dbg += " actor_name=";
				dbg += a->second;
			}
			if (const auto s = meta.find("session_id"); s != meta.end()) {
				dbg += " session_id=";
				dbg += s->second;
			}
			obs_log(LOG_DEBUG, "StreamingRecognize: %s", dbg.c_str());
		}

		const bool has_transcript = !item.transcript.empty();
		const bool has_lifecycle = !item.connection_status.empty();
		if (has_transcript || has_lifecycle) {
			std::lock_guard<std::mutex> lock(queue_mutex);
			results_queue.push(std::move(item));
		}

		if (item.connection_status == "error") {
			running_ = false;
			audio_queue_cv_.notify_all();
			break;
		}
	}

	running_ = false;
	audio_queue_cv_.notify_all();
}
