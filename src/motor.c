#define _POSIX_C_SOURCE 200809L

#include <tomcrypt.h>
#include <time.h>
#include <curl/curl.h>
#include "motor.h"
#include "jobs/board.h"
#include "jobs/handlers.h"
#include "jobs/scheduler/scheduler.h"
#include "util/ansi_escapes.h"
#include "util/util.h"
#include "plugin/manager.h"
#include "io/chat/chat.h"
#include "io/filesystem/filesystem.h"
#include "world/world.h"
#include "world/material/material.h"

sky_main_t sky_main = {
		.protocol = __MC_PRO__,
		.version = __MOTOR_VER__,
		.mcver = __MC_VER__,
		.console = {
				.name = "SERVER",
				.op = cmd_op_4,
				.permissions = {
						.bytes_per_element = sizeof(char*)
				}
		},
		.workers = {
				.count = 4,
				.vector = {
						.bytes_per_element = sizeof(sky_worker_t*)
				}
		},
		.status = sky_starting,

		.instructions = {
				.lock = PTHREAD_MUTEX_INITIALIZER,
				.vector = {
						.bytes_per_element = sizeof(sky_instruction_t)
				}
		},

		.entities = {
				.vector = {
						.bytes_per_element = sizeof(void*)
				},
				.nextID = {
						.bytes_per_element = sizeof(uint32_t)
				}
		},

		.worlds = {
			.bytes_per_element = sizeof(wld_world_t*)
		},
		
		.world = {
			.name = "world",
			.seed = 0
		},

		// listener
		.listener = {
				.address = {
					.port = 25565
				},
				.clients = {
					.lock = PTHREAD_MUTEX_INITIALIZER,
					.vector = {
						.bytes_per_element = sizeof(ltg_client_t*)
					},
					.nextID = {
						.bytes_per_element = sizeof(ltg_client_t*)
					}
				},
				.online = {
					.max = 20
				},
				.online_mode = true
		}

};

int main(int argCount, char* args[]) {

	struct timespec start;
	clock_gettime(CLOCK_REALTIME, &start);

	utl_setupConsole();

	log_info("//      //  //////  //////  //////  //////");
	log_info("////  ////  //  //    //    //  //  //  //");
	log_info("// //// //  //  //    //    //  //  //////");
	log_info("//  //  //  //  //    //    //  //  // // ");
	log_info("//      //  //////    //    //////  //  MC");
	log_info("%s", sky_main.version);
	log_info("");

#ifdef __MOTOR_UNSAFE__
	log_warn("You are using an unsafe version of MotorMC! This version may have game breaking bugs or experimental features and should not be used for production servers!");
	log_info("");
#endif

	curl_global_init(CURL_GLOBAL_DEFAULT);

	// register aes cipher
	register_cipher(&aes_enc_desc);

	if (fs_fileExists("server.json")) {

		yyjson_doc* server = yyjson_read_file("server.json", 0, NULL, NULL);

		if (server) {

			yyjson_val* server_obj = yyjson_doc_get_root(server);
			size_t i, i_max;
			yyjson_val *server_key, *server_val;
			yyjson_obj_foreach(server_obj, i, i_max, server_key, server_val) {
				const char* key = yyjson_get_str(server_key);
				uint32_t hash = utl_hash(key);
				switch (hash) {
				case 0xcbee16a7: // "worker_count"
					sky_main.workers.count = yyjson_get_uint(server_val);
					break;
				case 0x7c9c614a: // "port"
					sky_main.listener.address.port = yyjson_get_uint(server_val);
					break;
				case 0x8931d3dc: // "online-mode"
					sky_main.listener.online_mode = yyjson_get_bool(server_val);
					break;
				case 0x7c9abc59: // "motd"
					sky_main.motd = cht_fromJson(server_val);
					break;
				default:
					log_warn("Unknown value '%s' in server.json! (%x)", key, hash);
					break;
				}
			}

		} else {

			log_error("Could not read server.json file!! Is the format correct?");

		}

		yyjson_doc_free(server);

	} else {

		log_info("server.json not found! Generating one...");
		
		byte_t server_json[] = {
			0x7b, 0x0d, 0x0a, 0x09, 0x22, 0x77, 0x6f, 0x72, 0x6b, 0x65, 0x72, 0x5f,
			0x63, 0x6f, 0x75, 0x6e, 0x74, 0x22, 0x3a, 0x20, 0x34, 0x2c, 0x0d, 0x0a,
			0x09, 0x22, 0x70, 0x6f, 0x72, 0x74, 0x22, 0x3a, 0x20, 0x32, 0x35, 0x35,
			0x36, 0x35, 0x2c, 0x0d, 0x0a, 0x09, 0x22, 0x6f, 0x6e, 0x6c, 0x69, 0x6e,
			0x65, 0x2d, 0x6d, 0x6f, 0x64, 0x65, 0x22, 0x3a, 0x20, 0x74, 0x72, 0x75,
			0x65, 0x2c, 0x0d, 0x0a, 0x09, 0x22, 0x6d, 0x6f, 0x74, 0x64, 0x22, 0x3a,
			0x20, 0x7b, 0x0d, 0x0a, 0x09, 0x09, 0x22, 0x74, 0x65, 0x78, 0x74, 0x22,
			0x3a, 0x20, 0x22, 0x41, 0x20, 0x4d, 0x69, 0x6e, 0x65, 0x63, 0x72, 0x61,
			0x66, 0x74, 0x20, 0x73, 0x65, 0x72, 0x76, 0x65, 0x72, 0x22, 0x0d, 0x0a,
			0x09, 0x7d, 0x0d, 0x0a, 0x7d
		};

		FILE* file = fopen("server.json", "w+");

		if (file) {

			fwrite(server_json, sizeof(server_json), 1, file);

		} else {

			log_error("Could not open 'server.json' for writing!");

		}

		fclose(file);

	}

	// setup motd if null
	if (sky_main.motd == NULL) {
		sky_main.motd = cht_alloc();
		sky_main.motd->text = "A Minecraft server";
	}
	
	if (argCount != 0) {
		for (int i = 0; i < argCount; ++i) {
			if (args[i][0] == '-') {
				// TODO flags
			}
		}
	}

	// load startup plugins
	plg_onStartup();

	// TODO load world
	if (fs_dirExists(sky_main.world.name)) {
		log_info("Loading world %s...", sky_main.world.name);
		wld_world_t* world = wld_load(sky_main.world.name);
		utl_vectorPush(&sky_main.worlds, &world);
	} else {
		log_info("Generating world %s...", sky_main.world.name);
		wld_world_t* world = wld_new(sky_main.world.name, (sky_main.world.seed == 0 ? time(NULL) : sky_main.world.seed), WLD_NORMAL);
		utl_vectorPush(&sky_main.worlds, &world);
	}

	// load postworld plugins
	plg_onPostworld();

	// initiate socket
	ltg_init();

	// start main thread
	pthread_create(&sky_main.thread, NULL, t_sky_main, NULL);

	for (size_t i = 0; i < sky_main.workers.count; ++i) {

		sky_worker_t* worker = malloc(sizeof(sky_worker_t));
		pthread_mutex_init(&worker->world_lock, NULL);
		worker->id = i;

		utl_vectorPush(&sky_main.workers.vector, &worker);

		pthread_create(&worker->thread, NULL, t_sky_worker, worker);

	}

	struct timespec time_now;
	clock_gettime(CLOCK_REALTIME, &time_now);
	log_info("Done (%.3fs)! For help type 'help'", ((time_now.tv_sec * SKY_NANOS_PER_SECOND + time_now.tv_nsec) - (start.tv_sec * SKY_NANOS_PER_SECOND + start.tv_nsec)) / 1000000000.0f);

	// enter console loop, threads are started and such, nothing else needs to be done on this thread
	char in[256];
	for (;;) {
		if (fgets(in, 256, stdin) != NULL) {

			cmd_handle(in, &sky_main.console);

		} else {

			log_error("Could not read input from stdin!");
			sky_term();

		}
	}

}

void* t_sky_main(void* input) {

	sky_main.status = sky_running;

	struct timespec nextTick, currentTime, sleepTime;

	clock_gettime(CLOCK_MONOTONIC, &nextTick);

	while (sky_main.status == sky_running) {

		clock_gettime(CLOCK_MONOTONIC, &currentTime);

		if (sky_toNanos(currentTime) < sky_toNanos(nextTick)) {
			sleepTime.tv_sec = nextTick.tv_sec - currentTime.tv_sec;
			sleepTime.tv_nsec = nextTick.tv_nsec - currentTime.tv_nsec;
			if (sleepTime.tv_nsec < 0) {
				sleepTime.tv_nsec += SKY_NANOS_PER_SECOND;
				sleepTime.tv_sec -= 1;
			}
		} else {
			sleepTime.tv_sec = 0;
			sleepTime.tv_nsec = 0;
			if ((sky_toNanos(currentTime) - sky_toNanos(nextTick)) / SKY_NANOS_PER_TICK > SKY_SKIP_TICKS) {
				log_warn("Can't keep up! Is the server overloaded? Running %dms or %d ticks behind", (sky_toNanos(currentTime) - sky_toNanos(nextTick)) / 1000000, (sky_toNanos(currentTime) - sky_toNanos(nextTick)) / SKY_NANOS_PER_TICK);
				clock_gettime(CLOCK_MONOTONIC, &nextTick);
			}
		}

		nextTick.tv_nsec = nextTick.tv_nsec + SKY_NANOS_PER_TICK;
		if (nextTick.tv_nsec > SKY_NANOS_PER_SECOND) {
			nextTick.tv_nsec -= SKY_NANOS_PER_SECOND;
			nextTick.tv_sec += 1;
		}

		nanosleep(&sleepTime, NULL);

		// pause work
		for (size_t i = 0; i < sky_main.workers.vector.size; ++i) {
			sky_worker_t* worker = utl_vectorGetAs(sky_worker_t*, &sky_main.workers.vector, i);
			pthread_mutex_lock(&worker->world_lock);
		}

		// do tick stuff
		sch_tick();

		// resume works (unlock in reverse order)
		for (size_t i = sky_main.workers.vector.size - 1; i < sky_main.workers.vector.size; --i) {
			sky_worker_t* worker = utl_vectorGetAs(sky_worker_t*, &sky_main.workers.vector, i);
			pthread_mutex_unlock(&worker->world_lock);
		}

	}

	return input;

}

void* t_sky_worker(void* worker_ptr) {

	sky_worker_t* worker = worker_ptr;

	while (sky_main.status != sky_stopping) {

		// do work
		job_work(worker);

	}

	return NULL;

}

void __attribute__ ((noreturn)) sky_term() {

	sky_main.status = sky_stopping;

	// stop listening
	ltg_term();

	// join main thread
	pthread_join(sky_main.thread, NULL);

	// join to each worker

	// resume workers waiting for jobs
	job_add(NULL);

	for (size_t i = 0; i < sky_main.workers.vector.size; ++i) {

		sky_worker_t* worker = utl_vectorGetAs(sky_worker_t*, &sky_main.workers.vector, i);

		pthread_join(worker->thread, NULL);

	}

	// TODO unload world

	// disable plugins
	plg_onDisable();

	sky_main.status = sky_stopped;

	curl_global_cleanup();

	utl_restoreConsole();

	exit(0);

}
