#include <curl/curl.h>
#include "login.h"
#include "play.h"
#include "../../util/util.h"
#include "../../motor.h"
#include "../../io/logger/logger.h"
#include "../../crypt/random.h"

#include <yyjson.h>

struct {

	CURL* curl;
	pthread_mutex_t lock;

} phd_authRequest = {
	.curl = NULL,
	.lock = PTHREAD_MUTEX_INITIALIZER
};

size_t phd_authResponseWrite(void* ptr, size_t size, size_t nmemb, phd_authResponse_t* r) {
	
	size_t new_len = r->len + size * nmemb;
	r->ptr = realloc(r->ptr, new_len + 1);

	memcpy(r->ptr + r->len, ptr, size * nmemb);
	r->ptr[new_len] = '\0';
	r->len = new_len;

	return size*nmemb;

}

bool_t phd_login(ltg_client_t* client, pck_packet_t* packet) {

	pck_readVarInt(packet); // packet length
	int32_t id = pck_readVarInt(packet);

	switch (id) {
	case 0x00:
		return phd_handleLoginStart(client, packet);
	case 0x01:
		return phd_handleEncryptionResponse(client, packet);
	case 0x02:
		return phd_handleLoginPluginResponse(client, packet);
	default:
		log_warn("Received unknown packet %x in login state!", id);
		return false;
	}

}

bool_t phd_handleLoginStart(ltg_client_t* client, pck_packet_t* packet) {

	pck_allocString(username, packet);
	client->username.length = username_length;
	client->username.value = username;

	if (client->protocol != sky_main.protocol) {

		if (client->protocol < sky_main.protocol) {

			cht_component_t msg = cht_new;
			msg.text = "Outdated client! Please use " __MC_VER__;

			phd_sendDisconnectLogin(client, msg);

		} else {

			cht_component_t msg = cht_new;
			msg.text = "Outdated server! I'm still on " __MC_VER__;

			phd_sendDisconnectLogin(client, msg);

		}

		return false;
	}

	phd_sendEncryptionRequest(client);

	return true;

}

bool_t phd_handleEncryptionResponse(ltg_client_t* client, pck_packet_t* packet) {

	struct {
		int32_t length;
		byte_t bytes[128];
	} secret;

	// get shared secret
	secret.length = pck_readVarInt(packet);
	pck_readBytes(packet, secret.bytes, secret.length);

	// decrypt shared secret
	cry_decryptRSA(secret.bytes, secret.bytes, secret.length, &sky_main.listener.keypair);
	utl_reverseBytes(secret.bytes, secret.bytes, LTG_AES_KEY_LENGTH);
	
	// start encryption cypher
	int cres = cfb8_start(0, secret.bytes, secret.bytes, LTG_AES_KEY_LENGTH, 0, &client->encryption.key);
	if (cres != CRYPT_OK) {
		log_error("Could not start encryption cipher! Error code: %d", cres);
		return false;
	}
	client->encryption.enabled = true;

	struct {
		int32_t length;
		union {
			byte_t bytes[128];
			uint32_t key;
		};
	} verify;

	// get verify
	verify.length = pck_readVarInt(packet);
	pck_readBytes(packet, verify.bytes, verify.length);

	// decrypt and check verify
	cry_decryptRSA(verify.bytes, verify.bytes, verify.length, &sky_main.listener.keypair);
	if (verify.key != client->verify) {

		cht_component_t msg = cht_new;
		msg.text = "Failed RSA Challenge";

		phd_sendDisconnectLogin(client, msg);
		return false;

	}

	if (sky_main.listener.online_mode) {
		// auth with Mojang's servers...

		pthread_mutex_lock(&phd_authRequest.lock);

		if (phd_authRequest.curl == NULL) {
			phd_authRequest.curl = curl_easy_init();
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_TCP_FASTOPEN, 1);
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_IPRESOLVE, CURL_IPRESOLVE_V4);
			curl_easy_setopt(phd_authRequest.curl, CURLOPT_WRITEFUNCTION, phd_authResponseWrite);
		}
		CURLcode res;

		if (!phd_authRequest.curl) {
			pthread_mutex_unlock(&phd_authRequest.lock);
			log_error("Failed to initialize cURL");
			return false;
		}

		// create server_id hash
		byte_t server_id_hash[sha1_desc.hashsize];
		hash_state md;
		sha1_init(&md);
		sha1_process(&md, (byte_t*) "", 0);
		sha1_process(&md, secret.bytes, LTG_AES_KEY_LENGTH);
		sha1_process(&md, sky_main.listener.keypair.ASN1.bytes, sky_main.listener.keypair.ASN1.length);
		sha1_done(&md, server_id_hash);

		// create server_id string
		char server_id[(sha1_desc.hashsize << 1) + 2];
		utl_toMinecraftHex(server_id, server_id_hash, sha1_desc.hashsize);

		char request[157];
		sprintf(request, "https://sessionserver.mojang.com/session/minecraft/hasJoined?username=%s&serverId=%s", client->username.value, server_id);
		curl_easy_setopt(phd_authRequest.curl, CURLOPT_URL, request);

		// prepare response string
		phd_authResponse_t response;
		response.len = 0;
		response.ptr = malloc(1);
		response.ptr[0] = '\0';

		curl_easy_setopt(phd_authRequest.curl, CURLOPT_WRITEDATA, &response);

		res = curl_easy_perform(phd_authRequest.curl);
		if (res != CURLE_OK) {
			
			pthread_mutex_unlock(&phd_authRequest.lock);

			log_error("Could not authenticate client: %s", curl_easy_strerror(res));

			if (response.ptr != NULL)
				free(response.ptr);
			
			return false;

		}

		long http_code = 0;
		curl_easy_getinfo(phd_authRequest.curl, CURLINFO_RESPONSE_CODE, &http_code);

		pthread_mutex_unlock(&phd_authRequest.lock);

		if (http_code != 200) {

			if (response.ptr != NULL)
				free(response.ptr);

			cht_component_t msg = cht_new;
			msg.text = "Authentication failed";

			phd_sendDisconnectLogin(client, msg);
			return false;

		}

		yyjson_doc* auth = yyjson_read(response.ptr, response.len, 0);

		yyjson_val* auth_obj = yyjson_doc_get_root(auth);
		size_t i, i_max;
		yyjson_val *auth_key, *auth_val;
		yyjson_obj_foreach(auth_obj, i, i_max, auth_key, auth_val) {
			switch (i) {
				case 0: { // id
					utl_readHexBytes(client->uuid, yyjson_get_str(auth_val), 16);
					break;
				}
				case 1: { // username
					const char* auth_username = yyjson_get_str(auth_val);
					if (strcmp(client->username.value, auth_username) != 0) {
						// free old username
						free(client->username.value);

						// copy new username
						client->username.length = yyjson_get_len(auth_val);
						client->username.value = malloc(client->username.length);
						memcpy(client->username.value, auth_username, client->username.length);
					}
					break;
				}
				case 2: { // properties
					size_t j, j_max;
					yyjson_val* property_obj;
					yyjson_arr_foreach(auth_val, j, j_max, property_obj) {
						
						enum {
							none,
							textures
						} property_type = none;

						size_t k, k_max;
						yyjson_val *property_key, *property_val;
						yyjson_obj_foreach(property_obj, k, k_max, property_key, property_val) {
							switch (k) {
								case 0: { // name
									if (yyjson_equals_str(property_val, "textures")) {
										property_type = textures;
									}
									break;
								}
								case 1: { // value
									switch (property_type) {
										case none: {
											log_error("Property type has not been set, is the json response from the auth server curropted?");
											
											yyjson_doc_free(auth);
											free(response.ptr);
											return false;
										}
										case textures: {
											size_t textures_len = yyjson_get_len(property_val);
											client->textures.value = malloc(textures_len);
											memcpy(client->textures.value, yyjson_get_str(property_val), textures_len);
											break;
										}
									}
									break;
								}
								case 2: { // signature
									switch (property_type) {
										case none: {
											log_error("Property type has not been set, is the json response from the auth server curropted?");
											
											yyjson_doc_free(auth);
											free(response.ptr);
											return false;
										}
										case textures: {
											size_t signature_len = yyjson_get_len(property_val);
											client->textures.signature = malloc(signature_len);
											memcpy(client->textures.signature, yyjson_get_str(property_val), signature_len);
											break;
										}
									}
									break;
								}
							}
						}

					}
					break;
				}
			}
		}

		// free auth response and auth json doc
		yyjson_doc_free(auth);
		free(response.ptr);
	}

	// send login success packet
	phd_sendLoginSuccess(client);

	// switch to play state and join game
	client->state = ltg_play;
	phd_sendJoinGame(client);

	return true;

}

bool_t phd_handleLoginPluginResponse(ltg_client_t* client, pck_packet_t* packet) {

	assert(client != NULL);
	assert(packet != NULL);

	//TODO
	return false;

}

void phd_sendDisconnectLogin(ltg_client_t* client, cht_component_t component) {

	pck_inline(packet, 512, IO_BIG_ENDIAN);

	pck_padLength(packet);

	pck_writeVarInt(packet, 0x00);

	char chat[512];
	uint32_t chat_length = cht_write(&component, chat);
	pck_writeString(packet, chat, chat_length);

	pck_writeLength(packet);

	ltg_send(client, packet);

}

void phd_sendEncryptionRequest(ltg_client_t* client) {

	pck_inline(response, 256, IO_BIG_ENDIAN);

	// gaps for length later
	pck_padLength(response);

	// packet type 0x01
	pck_writeVarInt(response, 0x01);

	// server id
	if (sky_main.listener.online_mode) {
		pck_writeString(response, "", 0);
	} else {
		pck_writeString(response, "-", 1);
	}

	// the public auth_key
	pck_writeVarInt(response, sky_main.listener.keypair.ASN1.length);
	pck_writeBytes(response, sky_main.listener.keypair.ASN1.bytes, sky_main.listener.keypair.ASN1.length);

	// our verify token
	cry_randomBytes((uint8_t*) &client->verify, 4);
	pck_writeVarInt(response, 4);
	pck_writeInt32(response, client->verify);

	pck_writeLength(response);

	ltg_send(client, response);

}

void phd_sendLoginSuccess(ltg_client_t* client) {

	pck_inline(response, 32, IO_BIG_ENDIAN);

	// gaps for length later
	pck_padLength(response);

	pck_writeVarInt(response, 0x02);
	pck_writeBytes(response, client->uuid, 16);
	pck_writeString(response, client->username.value, client->username.length);

	pck_writeLength(response);

	ltg_send(client, response);

}

void phd_sendSetCompression(ltg_client_t* client) {

	assert(client != NULL);

}

void phd_sendLoginPluginRequest(ltg_client_t* client) {

	assert(client != NULL);

}
