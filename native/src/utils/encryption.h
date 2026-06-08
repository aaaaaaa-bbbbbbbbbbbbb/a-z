#pragma once

#include <string>
#include <vector>
#include <cstdint>

namespace encryption {

	std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext, const std::vector<uint8_t>& key);
	std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& key);

	std::string base64_encode(const std::vector<uint8_t>& data);
	std::vector<uint8_t> base64_decode(const std::string& str);

	std::vector<uint8_t> derive_key(const std::string& password, const std::vector<uint8_t>& salt);
}
