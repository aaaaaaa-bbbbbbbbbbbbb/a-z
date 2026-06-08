#include "http_server.h"
#include "engine.h"
#include "utils/config.h"

#include <csignal>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

namespace {
std::unique_ptr<Engine> g_engine;
std::unique_ptr<HttpServer> g_server;

std::string env_or(const char* key, const std::string& fallback){
	const char* value = std::getenv(key);
	return value && *value ? std::string(value) : fallback;
}

int env_int_or(const char* key, int fallback){
	const char* value = std::getenv(key);
	return value && *value ? std::stoi(value) : fallback;
}

void signal_handler(int sig){
	if(sig == SIGTERM || sig == SIGINT){
		if(g_server) g_server->stop();
		if(g_engine) g_engine->stop();
	}
}

void print_usage(const char* program){
	std::cerr << "Usage: " << program << " [options]\n"
			  << "  --config <file>\n"
			  << "  --algo <name>\n"
			  << "  --url <host:port>\n"
			  << "  --user <wallet/login>\n"
			  << "  --pass <password>\n"
			  << "  --rig-id <name>\n"
			  << "  --user-agent <agent>\n"
			  << "  --threads <n>\n"
			  << "  --max-cpu-usage <1..100>\n"
			  << "  --randomx-mode <fast|light>\n"
			  << "  --tls\n"
			  << "  --tls-no-verify\n"
			  << "  --api-port <port>\n"
			  << "  --http-access-token <token>\n";
}

Config base_config_from_env(){
	Config cfg;
	cfg.pool_url = env_or("EDGE_UPSTREAM", env_or("JOB_POOL", cfg.pool_url));
	cfg.pool_user = env_or("EDGE_WALLET", env_or("JOB_WALLET", cfg.pool_user));
	cfg.pool_pass = env_or("EDGE_POOL_PASS", cfg.pool_pass);
	cfg.rig_id = env_or("EDGE_INSTANCE_NAME", env_or("JOB_WORKER_NAME", cfg.rig_id));
	cfg.user_agent = env_or("EDGE_USER_AGENT", env_or("JOB_USER_AGENT", cfg.user_agent));
	cfg.threads = env_int_or("EDGE_THREADS", env_int_or("JOB_THREADS", cfg.threads));
	cfg.max_cpu_usage = env_int_or("EDGE_MAX_CPU_USAGE", env_int_or("JOB_MAX_CPU_USAGE", cfg.max_cpu_usage));
	cfg.randomx_mode = env_or("EDGE_RANDOMX_MODE", env_or("JOB_RANDOMX_MODE", cfg.randomx_mode));
	cfg.algorithm = env_or("EDGE_ALGORITHM", env_or("JOB_ALGORITHM", cfg.algorithm));
	cfg.api_token = env_or("EDGE_NODE_API_TOKEN", cfg.api_token);
	cfg.tls = env_or("EDGE_TLS", env_or("JOB_TLS", "true")) != "false";
	return cfg;
}
}

int main(int argc, char* argv[]){
	Config config = base_config_from_env();
	for(int i = 1; i < argc; ++i){
		const std::string arg = argv[i];
		auto next = [&](const char* name) -> std::string {
			if(i + 1 >= argc) throw std::invalid_argument(std::string(name) + " requires a value");
			return argv[++i];
		};
		if(arg == "--help" || arg == "-h"){ print_usage(argv[0]); return 0; }
		if(arg == "--config") config.load_from_file(next("--config"));
		else if(arg == "--algo") config.algorithm = next("--algo");
		else if(arg == "--url") config.pool_url = next("--url");
		else if(arg == "--user") config.pool_user = next("--user");
		else if(arg == "--pass") config.pool_pass = next("--pass");
		else if(arg == "--rig-id") config.rig_id = next("--rig-id");
		else if(arg == "--user-agent") config.user_agent = next("--user-agent");
		else if(arg == "--threads") config.threads = std::stoi(next("--threads"));
		else if(arg == "--max-cpu-usage") config.max_cpu_usage = std::stoi(next("--max-cpu-usage"));
		else if(arg == "--randomx-mode") config.randomx_mode = next("--randomx-mode");
		else if(arg == "--tls") config.tls = true;
		else if(arg == "--tls-no-verify") config.tls_verify = false;
		else if(arg == "--api-port") config.api_port = std::stoi(next("--api-port"));
		else if(arg == "--http-access-token") config.api_token = next("--http-access-token");
	}

	std::signal(SIGTERM, signal_handler);
	std::signal(SIGINT, signal_handler);

	try{
		config.validate();
		g_engine = std::make_unique<Engine>(config);
		g_server = std::make_unique<HttpServer>(config.api_port);
		g_server->add_route("GET", "/health", [](const std::string&){ return g_engine->health_json(); });
		g_server->add_route("GET", "/status", [](const std::string&){ return g_engine->health_json(); });
		g_server->add_route("GET", "/1/summary", [](const std::string&){ return g_engine->summary_json(); });
		g_server->start();
		g_engine->start();
		while(g_server->is_running() && g_engine->is_running()){
			std::this_thread::sleep_for(std::chrono::seconds(1));
		}
	}catch(const std::exception& e){
		std::cerr << "Fatal error: " << e.what() << std::endl;
		return 1;
	}
	return 0;
}
