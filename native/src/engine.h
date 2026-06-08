#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "pool_client.h"
#include "utils/config.h"

struct Share {
	std::string job_id;
	uint32_t nonce = 0;
	std::string nonce_hex;
	std::string result_hex;
};

class Engine {
public:
	explicit Engine(Config config);
	~Engine();

	void start();
	void stop();
	bool is_running() const;

	uint64_t get_hashrate() const;
	uint64_t get_total_hashes() const;
	uint64_t get_candidate_shares() const;
	uint64_t get_accepted_shares() const;
	uint64_t get_rejected_shares() const;
	std::string summary_json() const;
	std::string health_json() const;

private:
	struct RandomXBundle;

	void worker_thread(int thread_id);
	void pool_thread();
	void set_job(const StratumJob& job);
	void submit_share(const Share& share);
	std::shared_ptr<RandomXBundle> bundle_for_seed(const std::string& seed_hex);
	bool check_target(const uint8_t* hash, const std::string& target_hex) const;

	Config config_;
	std::atomic<bool> running_{false};
	std::atomic<bool> should_stop_{false};

	std::vector<std::thread> workers_;
	std::thread pool_thread_;

	std::atomic<uint64_t> total_hashes_{0};
	std::atomic<uint64_t> candidate_shares_{0};
	std::atomic<uint64_t> accepted_shares_{0};
	std::atomic<uint64_t> rejected_shares_{0};
	std::atomic<uint64_t> hashrate_{0};
	std::atomic<uint64_t> started_at_{0};
	std::unique_ptr<std::atomic<uint64_t>[]> thread_hashrates_;
	size_t thread_hashrates_count_ = 0;

	mutable std::mutex job_mutex_;
	StratumJob current_job_;
	std::atomic<uint64_t> job_version_{0};
	bool has_job_ = false;

	mutable std::mutex randomx_mutex_;
	std::shared_ptr<RandomXBundle> randomx_bundle_;

	std::unique_ptr<PoolClient> pool_client_;
};
