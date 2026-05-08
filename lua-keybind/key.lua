-- Ctrl+A 触发，创建 ctx
bind(CTRL, A, function(ctx)
	msg("Ctrl+A pressed, context created")

	-- 在 Ctrl 按住期间，按 B 触发
	ctx:bind(B, function()
		msg("Ctrl+A, then B")
	end)

	-- 在 Ctrl 按住期间，按 C 触发
	ctx:bind(C, function()
		msg("Ctrl+A, then C")
	end)
end)

-- Ctrl+D 触发，复用同一个 ctx
bind(CTRL, D, function(ctx)
	msg("Ctrl+D pressed, context created or reused")

	ctx:bind(E, function()
		msg("Ctrl+D, then E")
	end)

	ctx:bind(C, function()
		msg("Ctrl+D, then C")
	end)
end)

bind(CTRL, B, function(ctx)
	msg("Ctrl+B pressed directly")
end)
