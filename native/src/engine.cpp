#include "engine.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <json/json.h>
#include <limits>
#include <randomx.h>
#include <sstream>
#include <stdexcept>
#include <utility>
#include <random>
#if defined(__linux__)
#include <sys/random.h>
#endif

namespace {
uint64_t unix_now(){
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

std::vector<uint8_t> hex_to_bytes(const std::string& hex){
	if(hex.size() % 2 != 0) throw std::invalid_argument("hex length must be even");
	std::vector<uint8_t> out;
	out.reserve(hex.size() / 2);
	for(size_t i = 0; i < hex.size(); i += 2){
		const std::string b = hex.substr(i, 2);
		char* end = nullptr;
		const long v = std::strtol(b.c_str(), &end, 16);
		if(!end || *end != '\0' || v < 0 || v > 255) throw std::invalid_argument("invalid hex");
		out.push_back(static_cast<uint8_t>(v));
	}
	return out;
}

std::string bytes_to_hex(const uint8_t* data, size_t size){
	std::ostringstream oss;
	oss << std::hex << std::setfill('0');
	for(size_t i = 0; i < size; ++i) oss << std::setw(2) << static_cast<unsigned>(data[i]);
	return oss.str();
}

std::string nonce_to_hex_le(uint32_t nonce){
	uint8_t b[4] = {
		static_cast<uint8_t>(nonce & 0xff),
		static_cast<uint8_t>((nonce >> 8) & 0xff),
		static_cast<uint8_t>((nonce >> 16) & 0xff),
		static_cast<uint8_t>((nonce >> 24) & 0xff),
	};
	return bytes_to_hex(b, sizeof(b));
}

uint64_t read_le64(const uint8_t* p){
	uint64_t value = 0;
	for(int i = 0; i < 8; ++i) value |= static_cast<uint64_t>(p[i]) << (8 * i);
	return value;
}

uint32_t read_le32(const uint8_t* p){
	uint32_t value = 0;
	for(int i = 0; i < 4; ++i) value |= static_cast<uint32_t>(p[i]) << (8 * i);
	return value;
}

uint32_t secure_random_u32(){
	uint32_t value = 0;
#if defined(__linux__)
	const long got = static_cast<long>(getrandom(&value, sizeof(value), 0));
	if(got == static_cast<long>(sizeof(value)) && value != 0) return value;
#endif
	std::random_device rd;
	std::mt19937 gen(rd());
	return gen();
}

uint64_t pool_target_u64(const std::string& target_hex){
	const auto raw = hex_to_bytes(target_hex);
	if(raw.size() == 4){
		const uint32_t compact = read_le32(raw.data());
		if(compact == 0) return 0;
		const uint64_t denominator = 0xffffffffULL / static_cast<uint64_t>(compact);
		const uint64_t max = std::numeric_limits<uint64_t>::max();
		return denominator == 0 ? max : max / denominator;
	}
	if(raw.size() == 8) return read_le64(raw.data());
	return 0;
}

std::string json_compact(const Json::Value& value){
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	return Json::writeString(writer, value);
}
}

struct Engine::RandomXBundle {
	std::string seed_hex;
	randomx_flags flags = RANDOMX_FLAG_DEFAULT;
	randomx_cache* cache = nullptr;
	randomx_dataset* dataset = nullptr;

	~RandomXBundle(){
		if(dataset) randomx_release_dataset(dataset);
		if(cache) randomx_release_cache(cache);
	}
};

Engine::Engine(Config config) : config_(std::move(config)){}

Engine::~Engine(){ stop(); }

void Engine::start(){
	if(running_.exchange(true)) return;
	config_.validate();
	should_stop_ = false;
	started_at_ = unix_now();
	thread_hashrates_count_ = static_cast<size_t>(config_.threads);
	thread_hashrates_.reset(new std::atomic<uint64_t>[thread_hashrates_count_]);
	for(size_t i = 0; i < thread_hashrates_count_; ++i) thread_hashrates_[i] = 0;
	pool_client_ = std::make_unique<PoolClient>(config_.pool_url, config_.pool_user, config_.pool_pass,
												config_.rig_id, config_.user_agent, config_.tls, config_.tls_verify);
	pool_thread_ = std::thread(&Engine::pool_thread, this);
	for(int i = 0; i < config_.threads; ++i) workers_.emplace_back(&Engine::worker_thread, this, i);
}

void Engine::stop(){
	if(!running_.exchange(false)) return;
	should_stop_ = true;
	if(pool_client_) pool_client_->disconnect();
	for(auto& worker : workers_) if(worker.joinable()) worker.join();
	workers_.clear();
	if(pool_thread_.joinable()) pool_thread_.join();
}

bool Engine::is_running() const { return running_.load(); }
uint64_t Engine::get_hashrate() const { return hashrate_.load(); }
uint64_t Engine::get_total_hashes() const { return total_hashes_.load(); }
uint64_t Engine::get_candidate_shares() const { return candidate_shares_.load(); }
uint64_t Engine::get_accepted_shares() const { return pool_client_ ? pool_client_->accepted() : accepted_shares_.load(); }
uint64_t Engine::get_rejected_shares() const { return pool_client_ ? pool_client_->rejected() : rejected_shares_.load(); }

void Engine::set_job(const StratumJob& job){
	std::lock_guard<std::mutex> lock(job_mutex_);
	current_job_ = job;
	has_job_ = true;
	job_version_.fetch_add(1, std::memory_order_release);
}

void Engine::pool_thread(){
	while(!should_stop_){
		if(!pool_client_->connect_and_login()){
			std::this_thread::sleep_for(std::chrono::seconds(10));
			continue;
		}
		std::thread receiver([this](){
			pool_client_->receive_forever([this](const StratumJob& job){ set_job(job); }, should_stop_);
		});
		auto last_keepalive = std::chrono::steady_clock::now();
		while(!should_stop_ && pool_client_->is_connected()){
			const auto now = std::chrono::steady_clock::now();
			if(now - last_keepalive >= std::chrono::seconds(20)){
				pool_client_->keepalive();
				last_keepalive = now;
			}
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
		if(receiver.joinable()) receiver.join();
		pool_client_->disconnect();
		if(!should_stop_) std::this_thread::sleep_for(std::chrono::seconds(5));
	}
}

std::shared_ptr<Engine::RandomXBundle> Engine::bundle_for_seed(const std::string& seed_hex){
	std::lock_guard<std::mutex> lock(randomx_mutex_);
	if(randomx_bundle_ && randomx_bundle_->seed_hex == seed_hex) return randomx_bundle_;

	auto seed = hex_to_bytes(seed_hex);
	auto bundle = std::make_shared<RandomXBundle>();
	bundle->seed_hex = seed_hex;
	const randomx_flags recommended = randomx_get_flags();
	const int vm_mask = static_cast<int>(RANDOMX_FLAG_LARGE_PAGES) |
						static_cast<int>(RANDOMX_FLAG_HARD_AES) |
						static_cast<int>(RANDOMX_FLAG_JIT) |
						static_cast<int>(RANDOMX_FLAG_SECURE);
	bundle->flags = static_cast<randomx_flags>(static_cast<int>(recommended) & vm_mask);
	bundle->flags = static_cast<randomx_flags>(bundle->flags | RANDOMX_FLAG_HARD_AES | RANDOMX_FLAG_JIT);

	const bool fast = config_.randomx_mode != "light";
	if(fast) bundle->flags = static_cast<randomx_flags>(bundle->flags | RANDOMX_FLAG_FULL_MEM);

	const int cache_mask = static_cast<int>(RANDOMX_FLAG_LARGE_PAGES) |
						   static_cast<int>(RANDOMX_FLAG_JIT) |
						   static_cast<int>(RANDOMX_FLAG_ARGON2);
	const auto cache_flags = static_cast<randomx_flags>((static_cast<int>(recommended) | static_cast<int>(RANDOMX_FLAG_JIT)) & cache_mask);

	bundle->cache = randomx_alloc_cache(static_cast<randomx_flags>(static_cast<int>(cache_flags) | static_cast<int>(RANDOMX_FLAG_LARGE_PAGES)));
	if(!bundle->cache) bundle->cache = randomx_alloc_cache(cache_flags);
	if(!bundle->cache) throw std::runtime_error("randomx_alloc_cache failed");
	randomx_init_cache(bundle->cache, seed.data(), seed.size());

	if(fast){
		bundle->dataset = randomx_alloc_dataset(RANDOMX_FLAG_LARGE_PAGES);
		if(!bundle->dataset) bundle->dataset = randomx_alloc_dataset(RANDOMX_FLAG_DEFAULT);
		if(!bundle->dataset) throw std::runtime_error("randomx_alloc_dataset failed");

		const unsigned long total = randomx_dataset_item_count();
		unsigned init_threads = static_cast<unsigned>(std::max(1, config_.threads));
		const unsigned hw = std::thread::hardware_concurrency();
		if(hw > 0 && init_threads > hw) init_threads = hw;
		if(static_cast<unsigned long>(init_threads) > total) init_threads = static_cast<unsigned>(total);
		if(init_threads < 1) init_threads = 1;
		if(init_threads == 1){
			randomx_init_dataset(bundle->dataset, bundle->cache, 0, total);
		}else{
			const unsigned long per = total / init_threads;
			std::vector<std::thread> initers;
			initers.reserve(init_threads);
			for(unsigned t = 0; t < init_threads; ++t){
				const unsigned long start = static_cast<unsigned long>(t) * per;
				const unsigned long count = (t == init_threads - 1) ? (total - start) : per;
				randomx_cache* c = bundle->cache;
				randomx_dataset* d = bundle->dataset;
				initers.emplace_back([d, c, start, count](){
					randomx_init_dataset(d, c, start, count);
				});
			}
			for(auto& th : initers) th.join();
		}
	}
	randomx_bundle_ = bundle;
	return randomx_bundle_;
}

bool Engine::check_target(const uint8_t* hash, const std::string& target_hex) const {
	if(target_hex.empty()) return false;

	std::vector<uint8_t> raw;
	try{ raw = hex_to_bytes(target_hex); }catch(...){ return false; }
	if(raw.size() >= 32){
		for(int i = 31; i >= 0; --i){
			if(hash[i] < raw[i]) return true;
			if(hash[i] > raw[i]) return false;
		}
		return true;
	}

	uint64_t target64 = 0;
	try{ target64 = pool_target_u64(target_hex); }catch(...){ return false; }
	return target64 != 0 && read_le64(hash + 24) < target64;
}

void Engine::submit_share(const Share& share){
	if(!pool_client_ || !pool_client_->is_connected()) return;
	if(!pool_client_->submit_share(share.job_id, share.nonce_hex, share.result_hex)) rejected_shares_++;
}

void Engine::worker_thread(int thread_id){
	std::shared_ptr<RandomXBundle> bundle;
	randomx_vm* vm = nullptr;
	std::string vm_seed;
	uint64_t seen_job_version = 0;
	StratumJob job;
	std::vector<uint8_t> input;
	uint64_t local_hashes = 0;
	auto last_report = std::chrono::steady_clock::now();

	const uint32_t stride = static_cast<uint32_t>(std::max(1, config_.threads));
	uint32_t nonce = secure_random_u32() + static_cast<uint32_t>(thread_id);

	bool pipeline_open = false;
	uint32_t prev_nonce = 0;
	StratumJob prev_job;
	uint64_t work_ns = 0;

	auto reset_vm = [&](){
		if(vm){ randomx_destroy_vm(vm); vm = nullptr; }
	};
	auto place_nonce = [&](std::vector<uint8_t>& buf, uint32_t n){
		buf[config_.nonce_offset + 0] = static_cast<uint8_t>(n & 0xff);
		buf[config_.nonce_offset + 1] = static_cast<uint8_t>((n >> 8) & 0xff);
		buf[config_.nonce_offset + 2] = static_cast<uint8_t>((n >> 16) & 0xff);
		buf[config_.nonce_offset + 3] = static_cast<uint8_t>((n >> 24) & 0xff);
	};
	auto evaluate = [&](const StratumJob& src, uint32_t n, const uint8_t* hash){
		++local_hashes;
		++total_hashes_;
		if(check_target(hash, src.target_hex)){
			candidate_shares_++;
			Share share;
			share.job_id = src.job_id;
			share.nonce = n;
			share.nonce_hex = nonce_to_hex_le(n);
			share.result_hex = bytes_to_hex(hash, RANDOMX_HASH_SIZE);
			submit_share(share);
		}
	};

	while(!should_stop_){
		const uint64_t current_job_version = job_version_.load(std::memory_order_acquire);
		if(current_job_version != seen_job_version){
			std::lock_guard<std::mutex> lock(job_mutex_);
			const uint64_t locked_job_version = job_version_.load(std::memory_order_relaxed);
			if(has_job_ && seen_job_version != locked_job_version){
				job = current_job_;
				seen_job_version = locked_job_version;
			}
		}
		if(seen_job_version == 0 || job.blob.size() <= config_.nonce_offset + 3){
			pipeline_open = false;
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
		}
		if(!bundle || vm_seed != job.seed_hash_hex){
			reset_vm();
			pipeline_open = false;
			bundle = bundle_for_seed(job.seed_hash_hex);
			vm_seed = job.seed_hash_hex;
			const bool full_mem = (static_cast<int>(bundle->flags) & static_cast<int>(RANDOMX_FLAG_FULL_MEM)) != 0;
			randomx_cache* vm_cache = full_mem ? nullptr : bundle->cache;

			vm = randomx_create_vm(static_cast<randomx_flags>(static_cast<int>(bundle->flags) | static_cast<int>(RANDOMX_FLAG_LARGE_PAGES)), vm_cache, bundle->dataset);
			if(!vm) vm = randomx_create_vm(bundle->flags, vm_cache, bundle->dataset);
			if(!vm) throw std::runtime_error("randomx_create_vm failed");
		}

		if(!pipeline_open){
			input = job.blob;
			place_nonce(input, nonce);
			randomx_calculate_hash_first(vm, input.data(), input.size());
			prev_nonce = nonce;
			prev_job = job;
			pipeline_open = true;
			nonce += stride;
		}

		input = job.blob;
		place_nonce(input, nonce);
		uint8_t hash[RANDOMX_HASH_SIZE];
		const auto t0 = std::chrono::steady_clock::now();
		randomx_calculate_hash_next(vm, input.data(), input.size(), hash);
		const auto t1 = std::chrono::steady_clock::now();
		work_ns += static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
		evaluate(prev_job, prev_nonce, hash);
		prev_nonce = nonce;
		prev_job = job;
		nonce += stride;

		const auto now = std::chrono::steady_clock::now();
		const auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_report).count();
		if(elapsed >= 10){
			if(thread_id >= 0 && static_cast<size_t>(thread_id) < thread_hashrates_count_){
				thread_hashrates_[static_cast<size_t>(thread_id)] = static_cast<uint64_t>(local_hashes / static_cast<uint64_t>(elapsed));
			}
			uint64_t total_rate = 0;
			for(size_t i = 0; i < thread_hashrates_count_; ++i) total_rate += thread_hashrates_[i].load();
			hashrate_ = total_rate;
			local_hashes = 0;
			last_report = now;
		}

		if(config_.max_cpu_usage > 0 && config_.max_cpu_usage < 100 && work_ns >= 20000000ULL){
			const uint64_t idle = static_cast<uint64_t>(100 - config_.max_cpu_usage);
			const uint64_t sleep_ns = work_ns * idle / static_cast<uint64_t>(config_.max_cpu_usage);
			work_ns = 0;
			if(sleep_ns > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(sleep_ns));
		}
	}

	if(pipeline_open && vm){
		uint8_t hash[RANDOMX_HASH_SIZE];
		randomx_calculate_hash_last(vm, hash);
		evaluate(prev_job, prev_nonce, hash);
	}
	reset_vm();
}

std::string Engine::summary_json() const {
	Json::Value root;
	const uint64_t up = started_at_ == 0 ? 0 : unix_now() - started_at_.load();
	const uint64_t pool_up = pool_client_ ? pool_client_->connection_uptime() : 0;
	root["version"] = "edge-runtime/3.1-rxopt";
	root["kind"] = "custom-randomx";
	root["algo"] = config_.algorithm;
	root["uptime"] = Json::UInt64(up);
	root["hashrate"]["total"].append(Json::UInt64(hashrate_.load()));
	root["hashrate"]["total"].append(Json::UInt64(hashrate_.load()));
	root["hashrate"]["total"].append(Json::UInt64(hashrate_.load()));
	root["results"]["shares_good"] = Json::UInt64(pool_client_ ? pool_client_->accepted() : accepted_shares_.load());
	root["results"]["shares_total"] = Json::UInt64((pool_client_ ? pool_client_->accepted() + pool_client_->rejected() : accepted_shares_.load() + rejected_shares_.load()));
	root["results"]["shares_candidate"] = Json::UInt64(candidate_shares_.load());
	root["results"]["shares_submitted"] = Json::UInt64(pool_client_ ? pool_client_->submitted() : 0);
	root["connection"]["uptime"] = Json::UInt64(pool_up);
	root["connection"]["pool"] = pool_client_ ? pool_client_->pool() : config_.pool_url;
	root["connection"]["status"] = (pool_client_ && pool_client_->is_connected()) ? "connected" : "disconnected";
	root["resources"]["memory"]["resident_set"] = 0;
	root["cpu"]["threads"] = config_.threads;
	root["cpu"]["max_cpu_usage"] = config_.max_cpu_usage;
	return json_compact(root);
}

std::string Engine::health_json() const {
	Json::Value root;
	root["ok"] = running_.load();
	root["hashrate"] = Json::UInt64(hashrate_.load());
	root["connected"] = pool_client_ && pool_client_->is_connected();
	return json_compact(root);
}
