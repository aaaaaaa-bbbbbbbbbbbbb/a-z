#include "http_server.h"

#include <arpa/inet.h>
#include <cstring>
#include <iostream>
#include <netinet/in.h>
#include <stdexcept>
#include <sys/socket.h>
#include <unistd.h>

HttpServer::HttpServer(int port) : port_(port), server_fd_(-1){}
HttpServer::~HttpServer(){ stop(); }

void HttpServer::start(){
	if(running_.exchange(true)) return;
	server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
	if(server_fd_ < 0){
		running_ = false;
		throw std::runtime_error("failed to create HTTP socket");
	}
	int opt = 1;
	setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = INADDR_ANY;
	address.sin_port = htons(static_cast<uint16_t>(port_));
	if(bind(server_fd_, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0){
		close(server_fd_);
		server_fd_ = -1;
		running_ = false;
		throw std::runtime_error("failed to bind HTTP socket");
	}
	if(listen(server_fd_, 64) < 0){
		close(server_fd_);
		server_fd_ = -1;
		running_ = false;
		throw std::runtime_error("failed to listen on HTTP socket");
	}
	thread_ = std::thread(&HttpServer::server_thread, this);
}

void HttpServer::stop(){
	if(!running_.exchange(false)) return;
	if(server_fd_ >= 0){
		shutdown(server_fd_, SHUT_RDWR);
		close(server_fd_);
		server_fd_ = -1;
	}
	if(thread_.joinable()) thread_.join();
}

bool HttpServer::is_running() const { return running_.load(); }

void HttpServer::add_route(const std::string& method, const std::string& path, Handler handler){
	routes_[method + " " + path] = std::move(handler);
}

void HttpServer::server_thread(){
	while(running_){
		sockaddr_in address{};
		socklen_t addrlen = sizeof(address);
		const int client_fd = accept(server_fd_, reinterpret_cast<sockaddr*>(&address), &addrlen);
		if(client_fd < 0) continue;
		std::thread(&HttpServer::handle_request, this, client_fd).detach();
	}
}

void HttpServer::handle_request(int client_fd){
	char buffer[16384];
	const ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
	if(bytes_read <= 0){ close(client_fd); return; }
	buffer[bytes_read] = '\0';

	const std::string request(buffer);
	std::string method;
	std::string path;
	std::string body;

	const size_t first_line_end = request.find("\r\n");
	if(first_line_end != std::string::npos){
		const std::string first_line = request.substr(0, first_line_end);
		const size_t method_end = first_line.find(' ');
		const size_t path_end = first_line.find(' ', method_end + 1);
		if(method_end != std::string::npos && path_end != std::string::npos){
			method = first_line.substr(0, method_end);
			path = first_line.substr(method_end + 1, path_end - method_end - 1);
			const size_t query = path.find('?');
			if(query != std::string::npos) path = path.substr(0, query);
		}
	}
	const size_t body_start = request.find("\r\n\r\n");
	if(body_start != std::string::npos) body = request.substr(body_start + 4);

	const std::string response_body = process_request(method, path, body);
	const bool not_found = response_body == "__NOT_FOUND__";
	const std::string actual_body = not_found ? "{\"error\":\"not found\"}" : response_body;
	std::string response = not_found ? "HTTP/1.1 404 Not Found\r\n" : "HTTP/1.1 200 OK\r\n";
	response += "Content-Type: application/json\r\n";
	response += "Content-Length: " + std::to_string(actual_body.size()) + "\r\n";
	response += "Connection: close\r\n\r\n";
	response += actual_body;
	send(client_fd, response.data(), response.size(), 0);
	close(client_fd);
}

std::string HttpServer::process_request(const std::string& method, const std::string& path, const std::string& body){
	auto it = routes_.find(method + " " + path);
	if(it != routes_.end()) return it->second(body);
	return "__NOT_FOUND__";
}
