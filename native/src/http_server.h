#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <string>
#include <thread>

class HttpServer {
public:
	explicit HttpServer(int port);
	~HttpServer();

	void start();
	void stop();
	bool is_running() const;

	using Handler = std::function<std::string(const std::string& body)>;
	void add_route(const std::string& method, const std::string& path, Handler handler);

private:
	void server_thread();
	void handle_request(int client_fd);
	std::string process_request(const std::string& method, const std::string& path, const std::string& body);

	int port_;
	int server_fd_;
	std::atomic<bool> running_{false};
	std::thread thread_;
	std::map<std::string, Handler> routes_;
};
