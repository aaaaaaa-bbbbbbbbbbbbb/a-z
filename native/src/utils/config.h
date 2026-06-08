#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

struct Config {
	std::string config_file;

	std::string algorithm = "rx/0";
	std::string pool_url = "pool.supportxmr.com:443";
	std::string pool_user;
	std::string pool_pass = "x";
	std::string rig_id = "container-node";
	std::string user_agent = "Mozilla/5.0";

	int threads = 4;
	int max_cpu_usage = 100;
	std::string randomx_mode = "fast";
	bool tls = true;
	bool tls_verify = false;

	int api_port = 8081;
	std::string api_token = "edge-node-api-token";
	int log_level = 0;
	bool daemon = false;

	std::size_t nonce_offset = 39;

	void load_from_file(const std::string& path);
	void validate() const;
};
