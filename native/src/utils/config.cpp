#include "config.h"

#include <cstdlib>
#include <fstream>
#include <json/json.h>
#include <stdexcept>

namespace {
int bounded_int(const Json::Value& value, const char* name, int min, int max, int fallback){
	if(value.isNull()) return fallback;
	if(!value.isInt()) throw std::invalid_argument(std::string(name) + " must be an integer");
	const int n = value.asInt();
	if(n < min || n > max) throw std::invalid_argument(std::string(name) + " out of range");
	return n;
}

std::string string_value(const Json::Value& value, const std::string& fallback){
	return value.isString() ? value.asString() : fallback;
}
}

void Config::load_from_file(const std::string& path){
	config_file = path;
	std::ifstream file(path);
	if(!file.is_open()){
		throw std::runtime_error("could not open config file: " + path);
	}

	Json::Value root;
	Json::CharReaderBuilder builder;
	std::string errors;
	if(!Json::parseFromStream(builder, file, &root, &errors)){
		throw std::runtime_error("failed to parse config: " + errors);
	}

	algorithm = string_value(root["algorithm"], algorithm);
	threads = bounded_int(root["threads"], "threads", 1, 64, threads);
	max_cpu_usage = bounded_int(root["maxCpuUsage"], "maxCpuUsage", 1, 100, max_cpu_usage);
	randomx_mode = string_value(root["randomxMode"], randomx_mode);

	if(root.isMember("pool")){
		const auto& pool = root["pool"];
		pool_url = string_value(pool["url"], pool_url);
		pool_user = string_value(pool["user"], pool_user);
		pool_pass = string_value(pool["pass"], pool_pass);
		tls = pool.isMember("tls") ? pool["tls"].asBool() : tls;
		tls_verify = pool.isMember("tlsVerify") ? pool["tlsVerify"].asBool() : tls_verify;
	}

	if(root.isMember("api")){
		const auto& api = root["api"];
		api_port = bounded_int(api["port"], "api.port", 1, 65535, api_port);
		api_token = string_value(api["accessToken"], api_token);
	}
}

void Config::validate() const {
	if(algorithm.empty()) throw std::invalid_argument("algorithm is required");
	if(pool_url.empty()) throw std::invalid_argument("pool URL is required");
	if(pool_user.empty()) throw std::invalid_argument("wallet/login is required");
	if(threads < 1 || threads > 64) throw std::invalid_argument("threads must be 1..64");
	if(max_cpu_usage < 1 || max_cpu_usage > 100) throw std::invalid_argument("max CPU usage must be 1..100");
	if(api_port < 1 || api_port > 65535) throw std::invalid_argument("api port must be 1..65535");
	if(nonce_offset > 252) throw std::invalid_argument("nonce offset is invalid");
}
