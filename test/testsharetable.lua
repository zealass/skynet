local skynet = require "skynet"
local sharetable = require "skynet.sharetable"

skynet.start(function()
	sharetable.load("test", "return { x=1,y={2,3},z='hello world'}")
	local t = sharetable.query "test"
	for k,v in pairs(t) do
		print(k,v)
	end
end)
