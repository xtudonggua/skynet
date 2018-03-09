local skynet = require "skynet"
local gateserver = require "snax.wsgateserver"
local netpack = require "skynet.websocketnetpack"
require "skynet.manager"

local watchdog
local connection = {}	-- fd -> connection : { fd , client, agent , ip, mode }
local forwarding = {}	-- agent -> connection

skynet.register_protocol {
	name = "client",
	id = skynet.PTYPE_CLIENT,
}

local handler = {}

function handler.open(source, conf)
	-- watchdog = conf.watchdog or source
    print("-----> wslogind handler.open ", source)
end

function handler.message(fd, msg, sz)
	-- recv a package, forward it
	local c = connection[fd]
    print("-----> wslogind handler.message ", fd, msg, sz)
	--local agent = c.agent
	--if agent then
	--	skynet.redirect(agent, c.client, "client", 0, msg, sz)
	--else
	--	skynet.send(watchdog, "lua", "socket", "data", fd, netpack.tostring(msg, sz))
	--end
end

function handler.connect(fd, addr)
	local c = {
		fd = fd,
		ip = addr,
	}
	connection[fd] = c
    print("-----> wslogind handler.connect ", fd, addr)
    gateserver.openclient(fd)
	-- skynet.send(watchdog, "lua", "socket", "open", fd, addr)
end

local function unforward(c)
	if c.agent then
		forwarding[c.agent] = nil
		c.agent = nil
		c.client = nil
	end
end

local function close_fd(fd)
	local c = connection[fd]
	if c then
		unforward(c)
		connection[fd] = nil
	end
end

function handler.disconnect(fd)
	close_fd(fd)
    print("-----> wslogind handler.disconnect ", fd)
	-- skynet.send(watchdog, "lua", "socket", "close", fd)
end

function handler.error(fd, msg)
	close_fd(fd)
    print("-----> wslogind handler.error ", fd, msg)
	-- skynet.send(watchdog, "lua", "socket", "error", fd, msg)
end

function handler.warning(fd, size)
    print("-----> wslogind handler.warning ", fd, size)
	-- skynet.send(watchdog, "lua", "socket", "warning", fd, size)
end

local CMD = {}

function CMD.forward(source, fd, client, address)
	local c = assert(connection[fd])
	unforward(c)
	c.client = client or 0
	c.agent = address or source
	forwarding[c.agent] = c
	gateserver.openclient(fd)
end

function CMD.accept(source, fd)
	local c = assert(connection[fd])
	unforward(c)
	gateserver.openclient(fd)
end

function CMD.kick(source, fd)
	gateserver.closeclient(fd)
end

--
function CMD.send_buffer(source, fd, buffer, isText)
	gateserver.send_buffer(fd, buffer, isText)
end

function handler.command(cmd, source, ...)
	local f = assert(CMD[cmd])
	return f(source, ...)
end

skynet.register ".wslogind"
gateserver.start(handler)
