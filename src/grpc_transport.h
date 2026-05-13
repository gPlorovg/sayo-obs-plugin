#pragma once

#include "sayo.grpc.pb.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

struct RecognitionResult {
	std::string transcript;
	bool is_final = false;
	float confidence = 0.0f;
	/* From response.metadata["connection_status"] during session handshake (and "error"). */
	std::string connection_status;
	std::string connection_detail;
};

class ASRGrpcClient {
public:
	ASRGrpcClient(const std::string &server, int port);
	~ASRGrpcClient();

	bool HealthCheck(sayo::HealthCheckResponse &response, std::string &error_message);
	bool Start(const sayo::StreamingConfig &config);
	void Stop();
	void SendAudioChunk(std::vector<char> chunk);
	bool IsRunning() const;

	std::queue<RecognitionResult> results_queue;
	std::mutex queue_mutex;

private:
	void SenderLoop();
	void ReceiverLoop();

	std::shared_ptr<grpc::Channel> channel_;
	std::unique_ptr<sayo::SayoService::Stub> stub_;
	std::unique_ptr<grpc::ClientContext> context_;
	std::unique_ptr<grpc::ClientReaderWriter<sayo::StreamingRecognizeRequest, sayo::StreamingRecognizeResponse>> stream_;

	std::atomic<bool> running_{false};
	std::thread sender_thread_;
	std::thread receiver_thread_;

	mutable std::mutex audio_queue_mutex_;
	std::condition_variable audio_queue_cv_;
	std::queue<std::vector<char>> audio_queue_;

	sayo::StreamingConfig active_config_;
};
