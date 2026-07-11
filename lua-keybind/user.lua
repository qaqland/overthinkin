local selected = 1
local MAX_SCREEN = 8

local function clamp_screen(n)
	if n < 1 then
		return MAX_SCREEN
	end
	if n > MAX_SCREEN then
		return 1
	end
	return n
end

function next_screen()
	selected = clamp_screen(selected + 1)
	aa.preview(selected)
end

function prev_screen()
	selected = clamp_screen(selected - 1)
	aa.preview(selected)
end

function commit_screen()
	aa.commit(selected)
end

function reset_screen()
	selected = 1
	commit_screen()
end

local function setup_context_bindings()
	aa.bind(nil, nil, function()
		commit_screen()
	end)

	aa.bind(nil, "Tab", next_screen)
	aa.bind(SHIFT, "Tab", prev_screen)
	aa.bind(nil, "`", prev_screen)
end

aa.bind(CTRL, "Tab", function()
	next_screen()
	setup_context_bindings()
end)

aa.bind(CTRL + SHIFT, "Tab", function()
	prev_screen()
	setup_context_bindings()
end)

aa.bind(CTRL, "`", function()
	reset_screen()
end)

local function commit_screen_by_index(n)
	selected = n
	aa.commit(n)
end

for i = 1, MAX_SCREEN do
	aa.bind(CTRL, tostring(i), commit_screen_by_index, i)
end
