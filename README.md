fsnet
=====

<<<<<<< HEAD
这是一个, libevent + ruby 为基础的网游服务器. 可支持分布式节点部署
=======
一个c+ruby支持分布式部署开源的网络游戏库
每一个服务都可以做成一个独立的节点(比如(网关节点,登陆节点,地图节点) ,每个几点都可以自己独立的可执行协议.
每个节点可以相互通讯. 比如2个节点, ping pong

-0- 因为设计到一些配置文件,不好写完. 最好看dome吧：）
下面这2个节点,是可以部署在不同的物理机器上的：）


``` ruby
class GameServer
	PACK_TYPE_PING = 1
end

class PingNode < GameServer
	attr_reader :tick_task
	def on_start_complete
		super()
		@tick_task = scheduler_update(1.0, -1) do |dt|
			ping
		end
	end
	def ping
		response = Proc.new() do |result, data|
		  pong_time = result.input.read_uint32
		  print pong_time
		end
		
		os = FSOutputStream.new
		os.write_uint32(Time.now)
		cmd = Pack.create( Pack.generate_serial, PACK_TYPE_PING, os )
		
		node = find_node_by_name("pong_node")
		node.send_pack(crate_cmd, nil, response)
	end
	
end


class PongNode < GameServer


	def on_start_complete
		super
		@handle_map_configure[PACK_TYPE_PING] = "cmd_ping"
	end


	def cmd_ping(sender, pack)
		
		ping_time = pack.input.read_uint32
		
		os = FSOutputStream.new
		os.write_uint32(Time.now)
		ret = Pack.create( pack.serial, PACK_TYPE_PING, os )
		sender.send_pack(ret)
			
	end
	
	
end

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

