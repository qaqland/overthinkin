#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480
#define MAX_BINDS 256
#define MAX_LOCAL_BINDS 64
#define MAX_LOG_LINES 50

struct Mod {
	SDL_Keymod mask;
	const char *name;
};

const struct Mod Mods[] = {
	{SDL_KMOD_ALT, "ALT"},
	{SDL_KMOD_CTRL, "CTRL"},
	{SDL_KMOD_SHIFT, "SHIFT"},
	{SDL_KMOD_NONE, NULL},
};

struct KeyName {
	SDL_Keycode code;
	const char *name;
};

const struct KeyName KeyNames[] = {
	{SDLK_A, "A"}, {SDLK_B, "B"}, {SDLK_C, "C"},	    {SDLK_D, "D"},
	{SDLK_E, "E"}, {SDLK_F, "F"}, {SDLK_G, "G"},	    {SDLK_H, "H"},
	{SDLK_I, "I"}, {SDLK_J, "J"}, {SDLK_K, "K"},	    {SDLK_L, "L"},
	{SDLK_M, "M"}, {SDLK_N, "N"}, {SDLK_O, "O"},	    {SDLK_P, "P"},
	{SDLK_Q, "Q"}, {SDLK_R, "R"}, {SDLK_S, "S"},	    {SDLK_T, "T"},
	{SDLK_U, "U"}, {SDLK_V, "V"}, {SDLK_W, "W"},	    {SDLK_X, "X"},
	{SDLK_Y, "Y"}, {SDLK_Z, "Z"}, {SDLK_UNKNOWN, NULL},
};

struct LocalBind {
	SDL_Keycode key;
	int callback_ref;
};

struct KeyBindContext {
	SDL_Keymod trigger_mod;
	struct LocalBind binds[MAX_LOCAL_BINDS];
	int bind_count;
	int lua_ctx_ref;
	int active;
};

struct GlobalBind {
	SDL_Keymod mod;
	SDL_Keycode key;
	int callback_ref;
};

struct AppState {
	SDL_Window *window;
	SDL_Renderer *renderer;
	struct {
		SDL_Keymod mod;
		SDL_Keycode key;
	} current;
};

static struct GlobalBind global_binds[MAX_BINDS];
static int global_bind_count = 0;
static struct KeyBindContext *active_ctx = NULL;
static lua_State *global_L = NULL;

static char message[245];

static int set_msg(lua_State *L) {
	const char *msg = luaL_checkstring(L, 1);
	snprintf(message, sizeof(message), "%s", msg);
	SDL_Log("%s", msg);
	return 0;
}

static void expose_msg(lua_State *L) {
	lua_pushcfunction(L, set_msg);
	lua_setglobal(L, "msg");
}

static int mod_add(lua_State *L) {
	SDL_Keymod *a = lua_touserdata(L, 1);
	SDL_Keymod *b = lua_touserdata(L, 2);

	SDL_Keymod *c = lua_newuserdata(L, sizeof(*c));
	*c = *a | *b;

	luaL_getmetatable(L, "Mod");
	lua_setmetatable(L, -2);
	return 1;
}

static int mod_sub(lua_State *L) {
	SDL_Keymod *a = lua_touserdata(L, 1);
	SDL_Keymod *b = lua_touserdata(L, 2);

	SDL_Keymod *c = lua_newuserdata(L, sizeof(*c));
	*c = *a & ~(*b);

	luaL_getmetatable(L, "Mod");
	lua_setmetatable(L, -2);
	return 1;
}

static int mod_tostring(lua_State *L) {
	SDL_Keymod *v = lua_touserdata(L, 1);

	char name[1024] = {0};
	char *ptr = name;
	for (int i = 0; Mods[i].name; i++) {
		if (!(*v & Mods[i].mask)) {
			continue;
		}
		int w = snprintf(ptr, sizeof(name) - (ptr - name), "%s%s",
				 ptr == name ? "" : "+", Mods[i].name);
		ptr += w;
	}

	lua_pushstring(L, name);
	return 1;
}

static void expose_mods(lua_State *L) {
	luaL_newmetatable(L, "Mod");

	lua_pushcfunction(L, mod_add);
	lua_setfield(L, -2, "__add");

	lua_pushcfunction(L, mod_sub);
	lua_setfield(L, -2, "__sub");

	lua_pushcfunction(L, mod_tostring);
	lua_setfield(L, -2, "__tostring");

	lua_pop(L, 1);

	for (int i = 0; Mods[i].name; i++) {
		SDL_Keymod *e = lua_newuserdata(L, sizeof(*e));
		*e = Mods[i].mask;
		luaL_getmetatable(L, "Mod");
		lua_setmetatable(L, -2);
		lua_setglobal(L, Mods[i].name);
	}
}

static int key_tostring(lua_State *L) {
	SDL_Keycode *v = lua_touserdata(L, 1);

	for (int i = 0; KeyNames[i].name; i++) {
		if (KeyNames[i].code == *v) {
			lua_pushstring(L, KeyNames[i].name);
			return 1;
		}
	}

	lua_pushfstring(L, "Key(%d)", *v);
	return 1;
}

static void expose_keys(lua_State *L) {
	luaL_newmetatable(L, "Key");

	lua_pushcfunction(L, key_tostring);
	lua_setfield(L, -2, "__tostring");

	lua_pop(L, 1);

	for (int i = 0; KeyNames[i].name; i++) {
		SDL_Keycode *e = lua_newuserdata(L, sizeof(*e));
		*e = KeyNames[i].code;
		luaL_getmetatable(L, "Key");
		lua_setmetatable(L, -2);
		lua_setglobal(L, KeyNames[i].name);
	}
}

static struct KeyBindContext *create_context(SDL_Keymod mod) {
	if (active_ctx && active_ctx->trigger_mod == mod) {
		return active_ctx;
	}

	if (active_ctx) {
		for (int i = 0; i < active_ctx->bind_count; i++) {
			luaL_unref(global_L, LUA_REGISTRYINDEX,
				   active_ctx->binds[i].callback_ref);
		}
		luaL_unref(global_L, LUA_REGISTRYINDEX,
			   active_ctx->lua_ctx_ref);
		SDL_free(active_ctx);
	}

	active_ctx = SDL_calloc(1, sizeof(*active_ctx));
	active_ctx->trigger_mod = mod;
	active_ctx->active = 1;

	lua_newtable(global_L);
	active_ctx->lua_ctx_ref = luaL_ref(global_L, LUA_REGISTRYINDEX);

	lua_rawgeti(global_L, LUA_REGISTRYINDEX, active_ctx->lua_ctx_ref);
	luaL_getmetatable(global_L, "Context");
	lua_setmetatable(global_L, -2);

	return active_ctx;
}

static void destroy_context(void) {
	if (!active_ctx)
		return;

	for (int i = 0; i < active_ctx->bind_count; i++) {
		luaL_unref(global_L, LUA_REGISTRYINDEX,
			   active_ctx->binds[i].callback_ref);
	}

	luaL_unref(global_L, LUA_REGISTRYINDEX, active_ctx->lua_ctx_ref);
	free(active_ctx);
	active_ctx = NULL;
}

static int ctx_bind(lua_State *L) {
	if (!active_ctx) {
		luaL_error(L, "No active context");
		return 0;
	}

	SDL_Keycode *key = luaL_checkudata(L, 2, "Key");
	if (!lua_isfunction(L, 3)) {
		luaL_error(L, "Expected function as third argument");
		return 0;
	}

	for (int i = 0; i < active_ctx->bind_count; i++) {
		if (active_ctx->binds[i].key == *key) {
			luaL_unref(L, LUA_REGISTRYINDEX,
				   active_ctx->binds[i].callback_ref);
			lua_pushvalue(L, 3);
			active_ctx->binds[i].callback_ref =
				luaL_ref(L, LUA_REGISTRYINDEX);
			return 0;
		}
	}

	if (active_ctx->bind_count >= MAX_LOCAL_BINDS) {
		luaL_error(L, "Too many local binds");
		return 0;
	}

	lua_pushvalue(L, 3);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	active_ctx->binds[active_ctx->bind_count].key = *key;
	active_ctx->binds[active_ctx->bind_count].callback_ref = ref;
	active_ctx->bind_count++;

	return 0;
}

static int l_bind(lua_State *L) {
	SDL_Keymod *mod = luaL_checkudata(L, 1, "Mod");
	SDL_Keycode *key = luaL_checkudata(L, 2, "Key");

	if (!lua_isfunction(L, 3)) {
		luaL_error(L, "Expected function as third argument");
		return 0;
	}

	for (int i = 0; i < global_bind_count; i++) {
		if (global_binds[i].mod == *mod &&
		    global_binds[i].key == *key) {
			luaL_unref(L, LUA_REGISTRYINDEX,
				   global_binds[i].callback_ref);
			lua_pushvalue(L, 3);
			global_binds[i].callback_ref =
				luaL_ref(L, LUA_REGISTRYINDEX);
			return 0;
		}
	}

	if (global_bind_count >= MAX_BINDS) {
		luaL_error(L, "Too many global binds");
		return 0;
	}

	lua_pushvalue(L, 3);
	int ref = luaL_ref(L, LUA_REGISTRYINDEX);

	global_binds[global_bind_count].mod = *mod;
	global_binds[global_bind_count].key = *key;
	global_binds[global_bind_count].callback_ref = ref;
	global_bind_count++;

	return 0;
}

static void expose_context_type(lua_State *L) {
	luaL_newmetatable(L, "Context");

	lua_pushvalue(L, -1);
	lua_setfield(L, -2, "__index");

	lua_pushcfunction(L, ctx_bind);
	lua_setfield(L, -2, "bind");

	lua_pop(L, 1);
}

static void expose_bind(lua_State *L) {
	lua_pushcfunction(L, l_bind);
	lua_setglobal(L, "bind");
}

static void call_lua_callback(int callback_ref) {
	lua_rawgeti(global_L, LUA_REGISTRYINDEX, callback_ref);
	if (lua_isfunction(global_L, -1)) {
		if (active_ctx) {
			lua_rawgeti(global_L, LUA_REGISTRYINDEX,
				    active_ctx->lua_ctx_ref);
		} else {
			lua_pushnil(global_L);
		}
		if (lua_pcall(global_L, 1, 0, 0) != LUA_OK) {
			SDL_LogError(SDL_LOG_CATEGORY_APPLICATION,
				     "Lua callback error: %s",
				     lua_tostring(global_L, -1));
			lua_pop(global_L, 1);
		}
	} else {
		lua_pop(global_L, 1);
	}
}

static void handle_key_down(SDL_Keycode key, SDL_Keymod mod) {
	if (active_ctx) {
		for (int i = 0; i < active_ctx->bind_count; i++) {
			if (active_ctx->binds[i].key == key) {
				call_lua_callback(
					active_ctx->binds[i].callback_ref);
				return;
			}
		}
	}

	for (int i = 0; i < global_bind_count; i++) {
		if ((global_binds[i].mod & mod) &&
		    (global_binds[i].key == key)) {
			create_context(global_binds[i].mod);
			call_lua_callback(global_binds[i].callback_ref);
			return;
		}
	}
}

static SDL_Keymod keycode_to_mod(SDL_Keycode key) {
	switch (key) {
	case SDLK_LCTRL:
	case SDLK_RCTRL:
		return SDL_KMOD_CTRL;
	case SDLK_LALT:
	case SDLK_RALT:
		return SDL_KMOD_ALT;
	case SDLK_LSHIFT:
	case SDLK_RSHIFT:
		return SDL_KMOD_SHIFT;
	default:
		return SDL_KMOD_NONE;
	}
}

static void handle_key_up(SDL_Keycode key) {
	SDL_Keymod released_mod = keycode_to_mod(key);
	if (released_mod == SDL_KMOD_NONE)
		return;

	if (active_ctx && (active_ctx->trigger_mod & released_mod)) {
		message[0] = '\0';
		destroy_context();
	}
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
	(void) argc;
	(void) argv;

	SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	struct AppState *app = SDL_calloc(1, sizeof(*app));
	*appstate = app;

	if (!SDL_CreateWindowAndRenderer("Lua Keybind", WINDOW_WIDTH,
					 WINDOW_HEIGHT, 0, &app->window,
					 &app->renderer)) {
		SDL_Log("SDL_CreateWindowAndRenderer: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	SDL_SetRenderLogicalPresentation(app->renderer, WINDOW_WIDTH,
					 WINDOW_HEIGHT,
					 SDL_LOGICAL_PRESENTATION_LETTERBOX);

	global_L = luaL_newstate();
	luaL_openlibs(global_L);

	expose_msg(global_L);
	expose_mods(global_L);
	expose_keys(global_L);
	expose_context_type(global_L);
	expose_bind(global_L);

	const char *script_file = "key.lua";
	if (luaL_dofile(global_L, script_file) != LUA_OK) {
		SDL_Log("Lua error: %s", lua_tostring(global_L, -1));
		lua_pop(global_L, 1);
		return SDL_APP_FAILURE;
	}
	SDL_Log("Lua script loaded successfully");
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
	struct AppState *app = appstate;

	SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
	SDL_RenderClear(app->renderer);

	const float scale = 2.0f;
	SDL_SetRenderScale(app->renderer, scale, scale);
	SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);

	SDL_RenderDebugTextFormat(app->renderer, 10, 10, "%s", message);
	SDL_RenderPresent(app->renderer);

	SDL_Delay(10);
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	struct AppState *app = appstate;

	switch (event->type) {
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;

	case SDL_EVENT_KEY_DOWN:
		if (event->key.key == SDLK_ESCAPE) {
			return SDL_APP_SUCCESS;
		}
		app->current.mod = event->key.mod;
		app->current.key = event->key.key;
		if (event->key.repeat) {
			break;
		}
		handle_key_down(event->key.key, event->key.mod);
		break;

	case SDL_EVENT_KEY_UP:
		app->current.mod = event->key.mod;
		app->current.key = SDLK_UNKNOWN;
		handle_key_up(event->key.key);
		break;
	}

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	struct AppState *app = appstate;
	(void) result;

	destroy_context();

	for (int i = 0; i < global_bind_count; i++) {
		luaL_unref(global_L, LUA_REGISTRYINDEX,
			   global_binds[i].callback_ref);
	}

	if (global_L) {
		lua_close(global_L);
	}

	if (app->renderer) {
		SDL_DestroyRenderer(app->renderer);
	}

	if (app->window) {
		SDL_DestroyWindow(app->window);
	}

	SDL_free(appstate);
	SDL_Quit();
}
