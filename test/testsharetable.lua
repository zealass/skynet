local skynet = require "skynet"
local sharetable = require "skynet.sharetable"

skynet.start(function()
	-- make a short string first. "hello world" will output SSM, so it can't be shared
	print("hello " .. "world")
	sharetable.load("test", "return { x=1,y={ 'hello world' },['hello world'] = true }")
	local info = {}
	local t = sharetable.query("test", info)
	for k,v in pairs(info) do
		print(k,v)
	end
end)
