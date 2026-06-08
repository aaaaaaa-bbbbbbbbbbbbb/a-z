#include "pool_client.h"

#include <arpa/inet.h>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <json/json.h>
#include <memory>
#include <netdb.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <utility>

namespace {
uint64_t unix_now(){
	return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(
		std::chrono::system_clock::now().time_since_epoch()).count());
}

std::string json_compact(const Json::Value& value){
	Json::StreamWriterBuilder writer;
	writer["indentation"] = "";
	return Json::writeString(writer, value);
}

bool parse_json(const std::string& line, Json::Value& out){
	Json::CharReaderBuilder builder;
	std::string errors;
	std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
	return reader->parse(line.data(), line.data() + line.size(), &out, &errors);
}
}

PoolClient::PoolClient(std::string url, std::string user, std::string pass, std::string rig_id,
					   std::string user_agent, bool tls, bool verify_tls)
	: url_(std::move(url)), user_(std::move(user)), pass_(std::move(pass)),
	  rig_id_(std::move(rig_id)), user_agent_(std::move(user_agent)), tls_(tls), verify_tls_(verify_tls){
	parse_url(url_, tls_, host_, port_, tls_);
}

PoolClient::~PoolClient(){
	disconnect();
}

void PoolClient::parse_url(const std::string& url, bool tls_default, std::string& host, std::string& port, bool& tls){
	std::string value = url;
	tls = tls_default;
	const auto scheme = value.find(":
	if(scheme != std::string::npos){
		const std::string s = value.substr(0, scheme);
		tls = (s == "ssl" || s == "tls" || s == "stratum+ssl" || s == "stratum+tls");
		value = value.substr(scheme + 3);
	}
	const auto slash = value.find('/');
	if(slash != std::string::npos) value = value.substr(0, slash);
	const auto colon = value.rfind(':');
	if(colon == std::string::npos){
		host = value;
		port = tls ? "443" : "3333";
	}else{
		host = value.substr(0, colon);
		port = value.substr(colon + 1);
	}
	if(host.empty() || port.empty()) throw std::invalid_argument("invalid pool URL");
}

bool PoolClient::open_socket(){
	struct addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* result = nullptr;
	if(getaddrinfo(host_.c_str(), port_.c_str(), &hints, &result) != 0) return false;

	for(auto* p = result; p; p = p->ai_next){
		fd_ = socket(p->ai_family, p->ai_socktype, p->ai_protocol);
		if(fd_ < 0) continue;
		if(::connect(fd_, p->ai_addr, p->ai_addrlen) == 0) break;
		close(fd_);
		fd_ = -1;
	}
	freeaddrinfo(result);
	if(fd_ < 0) return false;

	timeval timeout{};
	timeout.tv_sec = 180;
	setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(fd_, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	if(tls_){
		SSL_library_init();
		SSL_load_error_strings();
		ssl_ctx_ = SSL_CTX_new(TLS_client_method());
		if(!ssl_ctx_) return false;
		SSL_CTX_set_default_verify_paths(ssl_ctx_);
		ssl_ = SSL_new(ssl_ctx_);
		if(!ssl_) return false;
		SSL_set_fd(ssl_, fd_);
		SSL_set_tlsext_host_name(ssl_, host_.c_str());
		if(verify_tls_) SSL_set1_host(ssl_, host_.c_str());
		SSL_set_verify(ssl_, verify_tls_ ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, nullptr);
		if(SSL_connect(ssl_) <= 0) return false;
		if(verify_tls_ && SSL_get_verify_result(ssl_) != X509_V_OK) return false;
	}
	connected_ = true;
	connected_since_ = unix_now();
	return true;
}

bool PoolClient::connect_and_login(){
	disconnect();
	if(!open_socket()){
		close_transport();
		return false;
	}
	if(!login()){
		close_transport();
		return false;
	}
	return true;
}

bool PoolClient::login(){
	Json::Value req;
	req["id"] = Json::UInt64(request_id_++);
	req["jsonrpc"] = "2.0";
	req["method"] = "login";
	req["params"]["login"] = user_;
	req["params"]["pass"] = pass_;
	req["params"]["agent"] = user_agent_;
	req["params"]["rigid"] = rig_id_;
	if(!send_json_locked(json_compact(req) + "\n")) return false;

	std::atomic<bool> no_stop{false};
	std::string line;
	while(read_line(line, no_stop)){
		Json::Value msg;
		if(!parse_json(line, msg)) continue;
		if(msg.isMember("error") && !msg["error"].isNull()) return false;
		const Json::Value result = msg["result"];
		if(result.isObject()){
			session_id_ = result["id"].asString();
			StratumJob initial;
			if(parse_job(result["job"], initial)){
				std::lock_guard<std::mutex> lock(initial_job_mutex_);
				initial_job_ = std::move(initial);
				has_initial_job_ = true;
			}
			return !session_id_.empty();
		}
	}
	return false;
}

void PoolClient::disconnect(){
	connected_ = false;
	close_transport();
}

void PoolClient::close_transport(){
	std::lock_guard<std::mutex> lock(io_mutex_);
	if(ssl_){ SSL_shutdown(ssl_); SSL_free(ssl_); ssl_ = nullptr; }
	if(ssl_ctx_){ SSL_CTX_free(ssl_ctx_); ssl_ctx_ = nullptr; }
	if(fd_ >= 0){ close(fd_); fd_ = -1; }
	read_buffer_.clear();
}

bool PoolClient::is_connected() const { return connected_.load(); }
std::string PoolClient::pool() const { return host_ + ":" + port_; }
uint64_t PoolClient::submitted() const { return submitted_.load(); }
uint64_t PoolClient::accepted() const { return accepted_.load(); }
uint64_t PoolClient::rejected() const { return rejected_.load(); }
uint64_t PoolClient::connection_uptime() const {
	if(!connected_) return 0;
	const uint64_t since = connected_since_.load();
	return since == 0 ? 0 : unix_now() - since;
}

bool PoolClient::write_all(const char* data, size_t size){
	size_t sent = 0;
	while(sent < size){
		const int n = tls_ ? SSL_write(ssl_, data + sent, static_cast<int>(size - sent))
						   : static_cast<int>(send(fd_, data + sent, size - sent, 0));
		if(n <= 0){ connected_ = false; return false; }
		sent += static_cast<size_t>(n);
	}
	return true;
}

int PoolClient::read_some(char* data, size_t size){
	const int n = tls_ ? SSL_read(ssl_, data, static_cast<int>(size))
					   : static_cast<int>(recv(fd_, data, size, 0));
	if(n <= 0) connected_ = false;
	return n;
}

bool PoolClient::send_json_locked(const std::string& payload){
	std::lock_guard<std::mutex> lock(io_mutex_);
	return write_all(payload.data(), payload.size());
}

bool PoolClient::read_line(std::string& line, const std::atomic<bool>& should_stop){
	line.clear();
	while(!should_stop.load()){
		const auto nl = read_buffer_.find('\n');
		if(nl != std::string::npos){
			line = read_buffer_.substr(0, nl);
			if(!line.empty() && line.back() == '\r') line.pop_back();
			read_buffer_.erase(0, nl + 1);
			return true;
		}
		char buf[4096];
		const int n = read_some(buf, sizeof(buf));
		if(n <= 0) return false;
		read_buffer_.append(buf, static_cast<size_t>(n));
		if(read_buffer_.size() > 1024 * 1024) return false;
	}
	return false;
}

bool PoolClient::submit_share(const std::string& job_id, const std::string& nonce_hex, const std::string& result_hex){
	const uint64_t id = request_id_++;
	Json::Value req;
	req["id"] = Json::UInt64(id);
	req["jsonrpc"] = "2.0";
	req["method"] = "submit";
	req["params"]["id"] = session_id_;
	req["params"]["job_id"] = job_id;
	req["params"]["nonce"] = nonce_hex;
	req["params"]["result"] = result_hex;
	{
		std::lock_guard<std::mutex> lock(pending_mutex_);
		pending_submit_ids_.insert(id);
	}
	if(!send_json_locked(json_compact(req) + "\n")){
		std::lock_guard<std::mutex> lock(pending_mutex_);
		pending_submit_ids_.erase(id);
		return false;
	}
	submitted_++;
	return true;
}

bool PoolClient::keepalive(){
	Json::Value req;
	req["id"] = Json::UInt64(request_id_++);
	req["jsonrpc"] = "2.0";
	req["method"] = "keepalived";
	req["params"]["id"] = session_id_;
	return send_json_locked(json_compact(req) + "\n");
}

void PoolClient::receive_forever(const JobCallback& callback, const std::atomic<bool>& should_stop){
	StratumJob initial;
	if(take_initial_job(initial)) callback(initial);
	std::string line;
	while(connected_ && read_line(line, should_stop)){
		handle_message(line, callback);
	}
	connected_ = false;
}

bool PoolClient::take_initial_job(StratumJob& job){
	std::lock_guard<std::mutex> lock(initial_job_mutex_);
	if(!has_initial_job_) return false;
	job = initial_job_;
	has_initial_job_ = false;
	return true;
}

bool PoolClient::parse_job(const Json::Value& value, StratumJob& job) const {
	if(!value.isObject()) return false;
	job.job_id = value["job_id"].asString();
	const std::string blob_hex = value["blob"].asString();
	job.target_hex = value["target"].asString();
	job.seed_hash_hex = value.isMember("seed_hash") ? value["seed_hash"].asString() : value["seed_hashes"][0].asString();
	job.height = value["height"].asUInt64();
	if(job.job_id.empty() || blob_hex.empty() || job.target_hex.empty() || job.seed_hash_hex.empty()) return false;
	job.blob.clear();
	if(blob_hex.size() % 2 != 0) return false;
	for(size_t i = 0; i < blob_hex.size(); i += 2){
		const std::string b = blob_hex.substr(i, 2);
		char* end = nullptr;
		const long v = std::strtol(b.c_str(), &end, 16);
		if(!end || *end != '\0' || v < 0 || v > 255) return false;
		job.blob.push_back(static_cast<uint8_t>(v));
	}
	return true;
}

void PoolClient::handle_message(const std::string& line, const JobCallback& callback){
	Json::Value msg;
	if(!parse_json(line, msg)) return;
	if(msg.isMember("method") && msg["method"].asString() == "job"){
		StratumJob job;
		if(parse_job(msg["params"], job)) callback(job);
		return;
	}
	if(msg.isMember("result") || msg.isMember("error")){
		const uint64_t id = msg["id"].asUInt64();
		bool is_submit = false;
		{
			std::lock_guard<std::mutex> lock(pending_mutex_);
			auto it = pending_submit_ids_.find(id);
			if(it != pending_submit_ids_.end()){
				pending_submit_ids_.erase(it);
				is_submit = true;
			}
		}
		if(!is_submit) return;
		if(msg.isMember("error") && !msg["error"].isNull()){
			rejected_++;
			return;
		}
		const Json::Value result = msg["result"];
		if(result.isObject() && result.isMember("status")){
			const std::string status = result["status"].asString();
			if(status == "OK" || status == "ok") accepted_++; else rejected_++;
		}else if(result.isBool() && result.asBool()){
			accepted_++;
		}
	}
}
