#include "encryption.h"
#include <openssl/evp.h>
#include <openssl/aes.h>
#include <openssl/rand.h>
#include <openssl/kdf.h>

namespace encryption {

std::vector<uint8_t> encrypt(const std::vector<uint8_t>& plaintext, const std::vector<uint8_t>& key){
	std::vector<uint8_t> ciphertext;

	std::vector<uint8_t> iv(12);
	RAND_bytes(iv.data(), iv.size());

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if(!ctx) return ciphertext;

	if(EVP_EncryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return ciphertext;
	}

	ciphertext.resize(plaintext.size() + AES_BLOCK_SIZE);
	int len;
	int ciphertext_len;

	if(EVP_EncryptUpdate(ctx, ciphertext.data(), &len, plaintext.data(), plaintext.size()) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return ciphertext;
	}
	ciphertext_len = len;

	if(EVP_EncryptFinal_ex(ctx, ciphertext.data() + len, &len) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return ciphertext;
	}
	ciphertext_len += len;

	std::vector<uint8_t> tag(16);
	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_GET_TAG, 16, tag.data());
	EVP_CIPHER_CTX_free(ctx);

	std::vector<uint8_t> result;
	result.insert(result.end(), iv.begin(), iv.end());
	result.insert(result.end(), ciphertext.begin(), ciphertext.begin() + ciphertext_len);
	result.insert(result.end(), tag.begin(), tag.end());

	return result;
}

std::vector<uint8_t> decrypt(const std::vector<uint8_t>& ciphertext, const std::vector<uint8_t>& key){
	if(ciphertext.size() < 28) return {};

	std::vector<uint8_t> iv(ciphertext.begin(), ciphertext.begin() + 12);
	std::vector<uint8_t> tag(ciphertext.end() - 16, ciphertext.end());
	std::vector<uint8_t> encrypted(ciphertext.begin() + 12, ciphertext.end() - 16);

	std::vector<uint8_t> plaintext(encrypted.size());

	EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
	if(!ctx) return {};

	if(EVP_DecryptInit_ex(ctx, EVP_aes_256_gcm(), nullptr, key.data(), iv.data()) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}

	EVP_CIPHER_CTX_ctrl(ctx, EVP_CTRL_GCM_SET_TAG, 16, tag.data());

	int len;
	int plaintext_len;

	if(EVP_DecryptUpdate(ctx, plaintext.data(), &len, encrypted.data(), encrypted.size()) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	plaintext_len = len;

	if(EVP_DecryptFinal_ex(ctx, plaintext.data() + len, &len) != 1){
		EVP_CIPHER_CTX_free(ctx);
		return {};
	}
	plaintext_len += len;

	EVP_CIPHER_CTX_free(ctx);

	plaintext.resize(plaintext_len);
	return plaintext;
}

std::string base64_encode(const std::vector<uint8_t>& data){
	static const char base64_chars[] =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::string encoded;
	int i = 0;
	uint8_t array3[3];
	uint8_t array4[4];

	for(uint8_t c : data){
		array3[i++] = c;
		if(i == 3){
			array4[0] = (array3[0] & 0xfc) >> 2;
			array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
			array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);
			array4[3] = array3[2] & 0x3f;

			for(int j = 0; j < 4; j++){
				encoded += base64_chars[array4[j]];
			}
			i = 0;
		}
	}

	if(i > 0){
		for(int j = i; j < 3; j++){
			array3[j] = '\0';
		}
		array4[0] = (array3[0] & 0xfc) >> 2;
		array4[1] = ((array3[0] & 0x03) << 4) + ((array3[1] & 0xf0) >> 4);
		array4[2] = ((array3[1] & 0x0f) << 2) + ((array3[2] & 0xc0) >> 6);

		for(int j = 0; j < i + 1; j++){
			encoded += base64_chars[array4[j]];
		}

		while(i++ < 3){
			encoded += '=';
		}
	}

	return encoded;
}

std::vector<uint8_t> base64_decode(const std::string& str){
	static const std::string base64_chars =
		"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

	std::vector<uint8_t> decoded;
	int i = 0;
	uint8_t array4[4];
	uint8_t array3[3];

	for(char c : str){
		if(c == '=') break;

		size_t pos = base64_chars.find(c);
		if(pos == std::string::npos) continue;

		array4[i++] = static_cast<uint8_t>(pos);
		if(i == 4){
			array3[0] = (array4[0] << 2) + ((array4[1] & 0x30) >> 4);
			array3[1] = ((array4[1] & 0x0f) << 4) + ((array4[2] & 0x3c) >> 2);
			array3[2] = ((array4[2] & 0x03) << 6) + array4[3];

			for(int j = 0; j < 3; j++){
				decoded.push_back(array3[j]);
			}
			i = 0;
		}
	}

	if(i > 0){
		for(int j = i; j < 4; j++){
			array4[j] = 0;
		}
		array3[0] = (array4[0] << 2) + ((array4[1] & 0x30) >> 4);
		array3[1] = ((array4[1] & 0x0f) << 4) + ((array4[2] & 0x3c) >> 2);
		array3[2] = ((array4[2] & 0x03) << 6) + array4[3];

		for(int j = 0; j < i - 1; j++){
			decoded.push_back(array3[j]);
		}
	}

	return decoded;
}

std::vector<uint8_t> derive_key(const std::string& password, const std::vector<uint8_t>& salt){
	std::vector<uint8_t> key(32);

	PKCS5_PBKDF2_HMAC(
		password.c_str(), password.length(),
		salt.data(), salt.size(),
		100000,
		EVP_sha256(),
		key.size(), key.data()
	);

	return key;
}

}
