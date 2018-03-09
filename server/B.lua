local skynet = require "skynet"
local harbor = require "skynet.harbor"

skynet.start(function()
    local srv = harbor.queryname("A1")
    local ret = skynet.call(srv, "lua", "123", "abc")
    print(ret)
end)
