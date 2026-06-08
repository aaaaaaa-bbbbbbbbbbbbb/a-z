#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <json/json.h>
#include <openssl/ssl.h>

struct StratumJob {
	std::string job_id;
	std::vector<uint8_t> blob;
	std::string target_hex;
	std::string seed_hash_hex;
	uint64_t height = 0;
};

class PoolClient {
public:
	using JobCallback = std::function<void(const StratumJob& job)>;

	PoolClient(std::string url, std::string user, std::string pass, std::string rig_id,
			   std::string user_agent, bool tls, bool verify_tls);
	~PoolClient();

	bool connect_and_login();
	void disconnect();
	bool is_connected() const;
	bool submit_share(const std::string& job_id, const std::string& nonce_hex, const std::string& result_hex);
	bool keepalive();
	bool take_initial_job(StratumJob& job);
	void receive_forever(const JobCallback& callback, const std::atomic<bool>& should_stop);

	std::string pool() const;
	uint64_t connection_uptime() const;
	uint64_t submitted() const;
	uint64_t accepted() const;
	uint64_t rejected() const;

private:
	bool open_socket();
	bool login();
	bool send_json_locked(const std::string& payload);
	bool read_line(std::string& line, const std::atomic<bool>& should_stop);
	bool write_all(const char* data, size_t size);
	int read_some(char* data, size_t size);
	void close_transport();
	void handle_message(const std::string& line, const JobCallback& callback);
	bool parse_job(const Json::Value& value, StratumJob& job) const;
	static void parse_url(const std::string& url, bool tls_default, std::string& host, std::string& port, bool& tls);

	std::string url_;
	std::string user_;
	std::string pass_;
	std::string rig_id_;
	std::string user_agent_;
	bool tls_ = true;
	bool verify_tls_ = true;
	std::string host_;
	std::string port_;

	int fd_ = -1;
	SSL_CTX* ssl_ctx_ = nullptr;
	SSL* ssl_ = nullptr;
	mutable std::mutex io_mutex_;
	std::string read_buffer_;
	std::atomic<bool> connected_{false};
	std::atomic<uint64_t> accepted_{0};
	std::atomic<uint64_t> rejected_{0};
	std::atomic<uint64_t> submitted_{0};
	std::atomic<uint64_t> connected_since_{0};
	std::string session_id_;
	std::atomic<uint64_t> request_id_{1};
	std::mutex pending_mutex_;
	std::set<uint64_t> pending_submit_ids_;
	std::mutex initial_job_mutex_;
	StratumJob initial_job_;
	bool has_initial_job_ = false;
};
