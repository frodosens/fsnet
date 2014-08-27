#fsnet

这是一个, libevent + ruby 为基础的网游服务器. 可支持分布式节点部署
每一个服务都可以做成一个独立的节点(比如(网关节点,登陆节点,地图节点) ,每个几点都可以自己独立的可执行协议.
每个节点可以相互通讯. 比如2个节点, ping pong

-0- 因为设计到一些配置文件,不好写完. 最好看dome吧：）

下面这2个节点,是可以部署在不同的物理机器上的：）


``` ruby
require 'game_server.rb'

class GameServer < GameTCPServer
	PACK_TYPE_PING = 1
end

class PingNode < GameServer
	attr_reader :tick_task
	def on_start_complete
		
		connect_node_by_configure("name" => "pong_node", "addr_ip" => "0.0.0.0", "addr_port" => 50001)
		
		@tick_task = scheduler_update(1.0, -1, Proc.new(){ |dt| ping });
		
	end
	def ping
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
pong_conf["base_configure"]["addr_ip"] = "0.0.0.0"
pong_conf["base_configure"]["addr_port"] = 50001
pong_conf["pack_handle"]["PACK_TYPE_PING"] = "cmd_ping"
$pong_node = PongNode.new(pong_conf)
$pong_node.start


ping_conf = {}
ping_conf["server_name"] = "PingServer"
ping_conf["base_configure"] = {}
ping_conf["base_configure"]["server_name"] = ping_conf["server_name"]
ping_conf["base_configure"]["addr_ip"] = "0.0.0.0"
ping_conf["base_configure"]["addr_port"] = 50000
$ping_node = PingNode.new(ping_conf)
$ping_node.start

``` 




# 如何编译,安装

#libevent
解压libs里的libevent-2.1.4-alpha.zip
cd libevent-2.1.4-alpha
./configure 
make
sudo make install

#ruby
解压libs 里的ruby-2.1.2.zip 
cd ruby-2.1.2
./configure --enable-shared
make
sudo make install

#jemalloc
解压jemalloc-3.6.0.tar.bz2
./configure
make
sudo make install

#fsnet
进入fsnet
执行
ruby extconf.rb
make & make install


#demo
进入scripts/ 
编辑server.rb
可以根据自己想看的demo取消注释. ：）

