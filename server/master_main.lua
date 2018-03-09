local skynet = require "skynet"

skynet.start(function()
    print("master start!!!")
    skynet.newservice("A")
end)
