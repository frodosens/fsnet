
require 'game_server.rb'

class GameServer < GameTCPServer
	PACK_TYPE_PING = 1
end

class PingNode < GameServer
	attr_reader :tick_task
	def on_start_complete
		
		connect_node_by_configure("name" => "pong_node", "addr_ip" => "0.0.0.0", "addr_port" => 50001)
		
		@tick_task = scheduler_update(1.0, -1, :ping);
		@tick_task2 = scheduler_update(0.016, -1, :tick);
		
	end
	
	def tick(sid, dt)

		os = FSOutputStream.new
		os.write_data("asdasdasd", 100)
		
		is = FSInputStream.new(os.data, os.len, true)
		p is.read_data(100	)
		GC.start
		
	end
	
	def ping(sid, dt)
		response = Proc.new() do |result, data|
		  pong_time = result.input.read_uint32
		  p "pong_time is #{pong_time}"
		end
		
		os = FSOutputStream.new
		os.write_uint32(Time.now.to_i)
		cmd = Pack.create( Pack.generate_serial, PACK_TYPE_PING, os )
		
		node = find_node_by_name("pong_node")
		node.send_pack(cmd, nil, response)
	end
	
end


class PongNode < GameServer
	
	def cmd_ping(sender, pack)
		
		ping_time = pack.input.read_uint32
		
		os = FSOutputStream.new
		os.write_uint32(Time.now.to_i)
		ret = Pack.create( pack.serial, PACK_TYPE_PING, os )
		sender.send_pack(ret)
			
	end
	
	
end



pong_conf = {}
pong_conf["server_name"] = "PongServer"
pong_conf["base_configure"] = {}
pong_conf["pack_handle"] = {}
pong_conf["base_configure"]["server_name"] = pong_conf["server_name"]
pong_conf["base_configure"]["addr_ip"] = "127.0.0.1"
pong_conf["base_configure"]["addr_port"] = 50001
pong_conf["pack_handle"]["PACK_TYPE_PING"] = "cmd_ping"
$pong_node = PongNode.new(pong_conf)
$pong_node.start


ping_conf = {}
ping_conf["server_name"] = "PingServer"
ping_conf["base_configure"] = {}
ping_conf["base_configure"]["server_name"] = ping_conf["server_name"]
ping_conf["base_configure"]["addr_ip"] = "127.0.0.1"
ping_conf["base_configure"]["addr_port"] = 50000
$ping_node = PingNode.new(ping_conf)
$ping_node.start




class Object
	
	class << self
		
		@@_objects = {}
		
	end
		
	def write_to_stream(fs)
		
		
		
	end
	
end





