#fsnet
===20141223======  
唔.发现如果想给太多的自由, 会导致非常迷惘. 完全不知道如何下手.  
1.更新一个典型的游戏服务器demo,   
2.加入一个简单的RPC调用框架(主要方便集群内通讯. 有幸得于Ruby的特性. RPC就像调用本地函数一样, 并且可以唔阻塞异步返回)  
3.ruby 还是那么的的漂亮. :P  

demo通过5个节点服务器组成  
1个中心服务器  
2个逻辑服务器  
2个网关服务器    

  
网关1 ->逻辑服务器1 \  
									中心服务器  
网关2 ->逻辑服务器2 /  
        

主张网关只负责网络.每个逻辑节点互不关联. 所有需要跨节点执行的. 都通过中心服务器转发.  
  
  
4. 这个框架我还是希望存的意义是最轻并且API优雅并美丽的通讯框架.  ( 上面那一套RPC调用框架+游戏服务器架构. core 部分没修改过, 只是一个扩展尝试, 只花了一个晚上.   结果还是非常让人开心.几乎没坑可以踩  )  
  
5. 圣诞快乐 ：）  
  
===
这是一个 C + ruby 为基础的网游服务器. 可支持分布式节点部署	
每一个服务都可以做成一个独立的节点(比如(网关节点,登陆节点,地图节点) 		  
每个节点都可以定义自己独立的可执行协议.  
每个节点可以相互通讯. 比如2个节点, ping pong				
下面这2个节点,是可以部署在不同的物理机器上的：）		  
---


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

