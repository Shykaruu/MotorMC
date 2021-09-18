#pragma once
#include "../../main.h"
#include "chat.h"

typedef enum {

	cht_translation_chat_type_text,
	cht_translation_multiplayer_player_joined,
	cht_translation_multiplayer_player_left,
	cht_translation_multiplayer_disconnect_outdated_client,
	cht_translation_multiplayer_disconnect_outdated_server,
	cht_translation_multiplayer_disconnect_server_shutdown

} cht_translation_type_t;

typedef struct {

	cht_translation_type_t translate;

	cht_color_t color;

	utl_vector_t with;

} cht_translation_t;

static /*in-plugin*/ const cht_translation_t cht_translation_new = {
	.color = cht_no_color,
	.with = UTL_VECTOR_INITIALIZER(cht_component_t*)
};

static inline void cht_add_with(cht_translation_t* translation, const cht_component_t* with) {
	utl_vector_push(&translation->with, &with);
}

extern void cht_jsonify_translation(mjson_doc*, mjson_val*, const cht_translation_t*);
extern size_t cht_write_translation(const cht_translation_t*, char*);

static inline void cht_term_translation(cht_translation_t* translation) {
	utl_term_vector(&translation->with);
}