local skynet = require "skynet"

skynet.start(function()
    skynet.newservice("wslogind")
    skynet.call(".wslogind", "lua", "open", {
        address = "0.0.0.0",
        port = 8888,
        maxclient = 1024,
        nodelay = true,
    })
    skynet.exit()
end)
