#define SDL_MAIN_USE_CALLBACKS 1
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>
#include <stdbool.h>

#define WINDOW_WIDTH 640
#define WINDOW_HEIGHT 480

#define LOCK_MASK (SDL_KMOD_CAPS | SDL_KMOD_NUM | SDL_KMOD_SCROLL)
#define MSG_INDICATOR_COUNT 8

struct AppState {
	SDL_Window *window;
	SDL_Renderer *renderer;
	lua_State *stack;
	int preview_index;
	int selected_index;
};

static SDL_Keymod normalize_mod(SDL_Keymod mod) {
	mod &= ~LOCK_MASK;
	if (mod & SDL_KMOD_CTRL)
		mod |= SDL_KMOD_CTRL;
	if (mod & SDL_KMOD_ALT)
		mod |= SDL_KMOD_ALT;
	if (mod & SDL_KMOD_GUI)
		mod |= SDL_KMOD_GUI;
	if (mod & SDL_KMOD_SHIFT)
		mod |= SDL_KMOD_SHIFT;
	return mod;
}

static SDL_Keymod context_mod_from_key(SDL_Keycode key) {
	switch (key) {
	case SDLK_LCTRL:
	case SDLK_RCTRL:
		return SDL_KMOD_CTRL;
	case SDLK_LALT:
	case SDLK_RALT:
		return SDL_KMOD_ALT;
	case SDLK_LGUI:
	case SDLK_RGUI:
		return SDL_KMOD_GUI;
	default:
		return SDL_KMOD_NONE;
	}
}

static int clamp_index(int n) {
	if (n < 1)
		return 1;
	if (n > MSG_INDICATOR_COUNT)
		return MSG_INDICATOR_COUNT;
	return n;
}

static int preview(lua_State *L) {
	struct AppState *app = lua_touserdata(L, lua_upvalueindex(1));
	int n = luaL_checkinteger(L, 1);
	app->preview_index = clamp_index(n);
	return 0;
}

static int commit(lua_State *L) {
	struct AppState *app = lua_touserdata(L, lua_upvalueindex(1));
	int n = luaL_checkinteger(L, 1);
	app->selected_index = clamp_index(n);
	app->preview_index = 0;
	return 0;
}

static void push_app_closure(lua_State *L, lua_CFunction fn, void *data) {
	lua_pushlightuserdata(L, data);
	lua_pushcclosure(L, fn, 1);
}

static void expose_module(lua_State *L, void *data) {
	lua_newtable(L);

	push_app_closure(L, preview, data);
	lua_setfield(L, -2, "preview");

	push_app_closure(L, commit, data);
	lua_setfield(L, -2, "commit");

	lua_setglobal(L, "aa");
}

static void expose_mods(lua_State *L) {
	lua_pushinteger(L, SDL_KMOD_CTRL);
	lua_setglobal(L, "CTRL");

	lua_pushinteger(L, SDL_KMOD_ALT);
	lua_setglobal(L, "ALT");

	lua_pushinteger(L, SDL_KMOD_GUI);
	lua_setglobal(L, "SUPER");

	lua_pushinteger(L, SDL_KMOD_SHIFT);
	lua_setglobal(L, "SHIFT");
}

static const char key_lua_source[] = {
#embed "key.lua"
	, 0};

static bool load_embedded_key_lua(lua_State *L) {
	size_t len = sizeof(key_lua_source) - 1;

	if (luaL_loadbuffer(L, key_lua_source, len, "key.lua") != LUA_OK) {
		SDL_Log("Failed to load key.lua: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}

	if (lua_pcall(L, 0, 0, 0) != LUA_OK) {
		SDL_Log("Failed to run key.lua: %s", lua_tostring(L, -1));
		lua_pop(L, 1);
		return false;
	}

	return true;
}

SDL_AppResult SDL_AppInit(void **appstate, int argc, char *argv[]) {
	(void) argc;
	(void) argv;

	SDL_SetLogPriority(SDL_LOG_CATEGORY_APPLICATION, SDL_LOG_PRIORITY_INFO);
	SDL_SetHint(SDL_HINT_MAIN_CALLBACK_RATE, "60");

	if (!SDL_Init(SDL_INIT_VIDEO)) {
		SDL_Log("SDL_Init failed: %s", SDL_GetError());
		return SDL_APP_FAILURE;
	}

	struct AppState *app = SDL_calloc(1, sizeof(*app));
	app->selected_index = 1;
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

	app->stack = luaL_newstate();
	luaL_openlibs(app->stack);

	expose_module(app->stack, app);
	expose_mods(app->stack);

	if (!load_embedded_key_lua(app->stack)) {
		return SDL_APP_FAILURE;
	}

	if (luaL_dofile(app->stack, "user.lua") != LUA_OK) {
		SDL_Log("Lua error: %s", lua_tostring(app->stack, -1));
		lua_pop(app->stack, 1);
		return SDL_APP_FAILURE;
	}

	SDL_Log("Lua script loaded successfully");
	return SDL_APP_CONTINUE;
}

SDL_AppResult SDL_AppIterate(void *appstate) {
	struct AppState *app = appstate;

	SDL_SetRenderDrawColor(app->renderer, 0, 0, 0, 255);
	SDL_RenderClear(app->renderer);

	SDL_SetRenderDrawColor(app->renderer, 255, 255, 255, 255);

	const float text_scale = 2.0f;
	const float text_y = 40.0f;
	const float text_x = 40.0f;
	const float line_h = SDL_DEBUG_TEXT_FONT_CHARACTER_SIZE * 1.6;

	SDL_SetRenderScale(app->renderer, text_scale, text_scale);
	SDL_RenderDebugTextFormat(app->renderer, text_x, text_y + line_h * 0,
				  ">>> lua-keybind");

	SDL_RenderDebugTextFormat(app->renderer, text_x, text_y + line_h * 2,
				  "Ctrl       Tab       next");
	SDL_RenderDebugTextFormat(app->renderer, text_x, text_y + line_h * 3,
				  "Ctrl+Shift Tab       prev");
	SDL_RenderDebugTextFormat(app->renderer, text_x, text_y + line_h * 4,
				  "Ctrl       `  (ctx)  prev");
	SDL_RenderDebugTextFormat(app->renderer, text_x, text_y + line_h * 5,
				  "Ctrl       `  (idle) reset");
	SDL_SetRenderScale(app->renderer, 1.0f, 1.0f);

	const float bar_width = WINDOW_WIDTH * 0.8f;
	const int gap = 4;
	const float bottom_margin = 40.0f;
	const float rect_w = (bar_width - (MSG_INDICATOR_COUNT - 1) * gap) /
			     MSG_INDICATOR_COUNT;
	const float rect_h = rect_w * 3.0f / 4.0f;
	const float start_x = (WINDOW_WIDTH - bar_width) / 2.0f;
	const float y = WINDOW_HEIGHT - rect_h - bottom_margin;

	for (int i = 0; i < MSG_INDICATOR_COUNT; i++) {
		SDL_FRect rect = {
			.x = start_x + i * (rect_w + gap),
			.y = y,
			.w = rect_w,
			.h = rect_h,
		};
		int idx = i + 1;
		bool is_selected = (idx == app->selected_index);
		bool is_preview = (idx == app->preview_index);

		if (is_selected) {
			SDL_SetRenderDrawColor(app->renderer, 255, 255, 255,
					       255);
			SDL_RenderFillRect(app->renderer, &rect);
		} else if (is_preview) {
			SDL_SetRenderDrawColor(app->renderer, 128, 128, 128,
					       255);
			SDL_RenderFillRect(app->renderer, &rect);
			SDL_SetRenderDrawColor(app->renderer, 255, 255, 255,
					       255);
			SDL_RenderRect(app->renderer, &rect);
		} else {
			SDL_SetRenderDrawColor(app->renderer, 255, 255, 255,
					       255);
			SDL_RenderRect(app->renderer, &rect);
		}
	}

	SDL_RenderPresent(app->renderer);

	return SDL_APP_CONTINUE;
}

static void call_lua_key_handler(lua_State *L, const char *fn_name,
				 const char *key_name, SDL_Keymod mod) {
	lua_getglobal(L, fn_name);
	if (!lua_isfunction(L, -1)) {
		lua_pop(L, 1);
		return;
	}

	lua_pushstring(L, key_name);
	lua_pushinteger(L, mod);

	if (lua_pcall(L, 2, 0, 0) != LUA_OK) {
		SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "Lua error: %s",
			     lua_tostring(L, -1));
		lua_pop(L, 1);
	}
}

SDL_AppResult SDL_AppEvent(void *appstate, SDL_Event *event) {
	struct AppState *app = appstate;

	switch (event->type) {
	case SDL_EVENT_QUIT:
		return SDL_APP_SUCCESS;
	case SDL_EVENT_KEY_DOWN:
		if (event->key.key == SDLK_ESCAPE)
			return SDL_APP_SUCCESS;
		if (event->key.repeat)
			break;
		call_lua_key_handler(app->stack, "on_key_down",
				     SDL_GetKeyName(event->key.key),
				     normalize_mod(event->key.mod));
		break;
	case SDL_EVENT_KEY_UP:
		call_lua_key_handler(app->stack, "on_key_up",
				     SDL_GetKeyName(event->key.key),
				     context_mod_from_key(event->key.key));
		break;
	}

	return SDL_APP_CONTINUE;
}

void SDL_AppQuit(void *appstate, SDL_AppResult result) {
	struct AppState *app = appstate;
	(void) result;

	if (app->stack)
		lua_close(app->stack);
	if (app->renderer)
		SDL_DestroyRenderer(app->renderer);
	if (app->window)
		SDL_DestroyWindow(app->window);

	SDL_free(app);
	SDL_Quit();
}
