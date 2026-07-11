local LOCK_MASK = 0x7000 -- SDL_KMOD_CAPS | SDL_KMOD_NUM | SDL_KMOD_SCROLL

local ModMeta = {
	__add = function(a, b)
		return setmetatable({ mask = a.mask | b.mask, _is_mod = true }, ModMeta)
	end,
	__sub = function(a, b)
		return setmetatable({ mask = a.mask & ~b.mask, _is_mod = true }, ModMeta)
	end,
	__eq = function(a, b)
		return a.mask == b.mask
	end,
	__tostring = function(m)
		local parts = {}
		if m.mask & CTRL.mask ~= 0 then
			table.insert(parts, "CTRL")
		end
		if m.mask & ALT.mask ~= 0 then
			table.insert(parts, "ALT")
		end
		if m.mask & SUPER.mask ~= 0 then
			table.insert(parts, "SUPER")
		end
		if m.mask & SHIFT.mask ~= 0 then
			table.insert(parts, "SHIFT")
		end
		return table.concat(parts, "+")
	end,
}

local function make_mod(mask)
	return setmetatable({ mask = mask, _is_mod = true }, ModMeta)
end

-- C 侧已经暴露 CTRL/ALT/SUPER/SHIFT 为整数，这里包装成 modifier 对象。
CTRL = make_mod(CTRL)
ALT = make_mod(ALT)
SUPER = make_mod(SUPER)
SHIFT = make_mod(SHIFT)

local function normalize_mod(mod)
	mod = mod & ~LOCK_MASK
	if mod & CTRL.mask ~= 0 then
		mod = mod | CTRL.mask
	end
	if mod & ALT.mask ~= 0 then
		mod = mod | ALT.mask
	end
	if mod & SUPER.mask ~= 0 then
		mod = mod | SUPER.mask
	end
	if mod & SHIFT.mask ~= 0 then
		mod = mod | SHIFT.mask
	end
	return mod
end

local function context_mod_from_shortcut(mod)
	mod = normalize_mod(mod)
	if mod & CTRL.mask ~= 0 then
		return CTRL.mask
	end
	if mod & ALT.mask ~= 0 then
		return ALT.mask
	end
	if mod & SUPER.mask ~= 0 then
		return SUPER.mask
	end
	return 0
end

local function has_multiple_primary_mods(mod)
	mod = normalize_mod(mod)
	local count = 0
	if mod & CTRL.mask ~= 0 then
		count = count + 1
	end
	if mod & ALT.mask ~= 0 then
		count = count + 1
	end
	if mod & SUPER.mask ~= 0 then
		count = count + 1
	end
	return count > 1
end

local pub_binds = {}
local ctx_binds = {}
local ctx_mod = 0
local ctx_free_callback = nil

local function context_active()
	return ctx_mod ~= 0
end

local function get_bind(table, mod_val, key)
	local t = table[mod_val]
	if t then
		return t[key]
	end
	return nil
end

local function set_bind(table, mod_val, key, value)
	if not table[mod_val] then
		table[mod_val] = {}
	end
	table[mod_val][key] = value
end

local function remove_bind(table, mod_val, key)
	local t = table[mod_val]
	if t then
		t[key] = nil
		if next(t) == nil then
			table[mod_val] = nil
		end
	end
end

local function clear_binds(table)
	for mod_val, _ in pairs(table) do
		table[mod_val] = nil
	end
end

local function call_handler(handler)
	if type(handler) == "table" then
		handler[1](table.unpack(handler, 2))
	elseif type(handler) == "function" then
		handler()
	end
end

local function get_mod_arg(obj)
	if obj == nil then
		return 0
	end
	if type(obj) == "table" and obj._is_mod then
		return normalize_mod(obj.mask)
	end
	error("Expected modifier or nil")
end

-- nil ~= nop
aa.nop = function() end

aa.bind = function(mod, key, fn, ...)
	if key == nil then
		if mod ~= nil then
			error("aa.bind(nil, nil, fn) expected")
		end
		if not context_active() then
			error("free callback requires active context")
		end
		if fn == nil then
			ctx_free_callback = nil
			return
		end
		if type(fn) ~= "function" then
			error("Expected function or nil")
		end
		ctx_free_callback = select("#", ...) > 0 and { fn, ... } or fn
		return
	end

	local mod_val = get_mod_arg(mod)
	if type(key) ~= "string" then
		error("Expected key name string")
	end

	local binds = pub_binds
	if context_active() then
		binds = ctx_binds
		mod_val = normalize_mod(ctx_mod | mod_val)
	end

	if has_multiple_primary_mods(mod_val) then
		error("Multiple primary modifiers cannot be bound together")
	end

	if fn == nil then
		remove_bind(binds, mod_val, key)
		return
	end

	if type(fn) ~= "function" then
		error("Expected function or nil")
	end

	local handler = select("#", ...) > 0 and { fn, ... } or fn
	set_bind(binds, mod_val, key, handler)
end

local function create_context(mod)
	if context_active() then
		return
	end
	ctx_mod = context_mod_from_shortcut(mod)
end

local function destroy_context()
	if not context_active() then
		return
	end

	clear_binds(ctx_binds)
	ctx_mod = 0

	local cb = ctx_free_callback
	ctx_free_callback = nil
	if cb then
		call_handler(cb)
	end
end

function on_key_down(key, mod)
	local clean = normalize_mod(mod)

	if context_active() then
		local handler = get_bind(ctx_binds, clean, key)
		if handler then
			call_handler(handler)
			return
		end
	end

	local handler = get_bind(pub_binds, clean, key)
	if handler then
		create_context(clean)
		call_handler(handler)
	end
end

function on_key_up(key, released_mod)
	if not context_active() then
		return
	end
	if released_mod ~= 0 and released_mod == ctx_mod then
		destroy_context()
	end
end
