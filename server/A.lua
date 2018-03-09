local skynet = require "skynet"
local harbor = require "skynet.harbor"

skynet.dispatch("lua", function(session, source, ...)
    print("A = ", session, source, ...)
    skynet.ret(skynet.pack("B"))
end)

skynet.start(function()
    local harbor_id = tonumber(skynet.getenv("harbor"))
    print("harbor_id = ", harbor_id)
    harbor.globalname("A" .. harbor_id)
    skynet.fork(function()
        local ret
        if harbor_id == 2 then
            ret = harbor.link(3)
        elseif harbor_id == 3 then
            ret = harbor.link(2)
        end
        print("ret = ", ret)
    end)
end)
