local skynet = require "skynet"

skynet.start(function()
    print("slave start!!!")
    skynet.newservice("A")
end)
