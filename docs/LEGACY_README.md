#fsnet
===20160704======  
放弃之前的设计, 重新为游戏服务器设计API.  
加入协程做同步RPC调用, 不再需要写任何callback.  
现在每一个invoke都是在一个独立的fiber上执行, 可以随时yield,再resume.  
发现一旦API设计的太过抽象, 会导致API特难用, 除非特别熟悉.  
我的期望看到函数名字就能知道干嘛 是最好的.  
  
新设计如下.  
  
分3个类型的server  
gate  
logic  
center  
  
最小执行单元是一个service  

一个logic可以承载N个service  
logic在启动的时候会把自身承载的service通知给gate和center  
当gate收到客户端的service消息的时候直接发送到对应的logic进程上  
center会做一个类似地址表的东西 会标记 哪一个service在哪一个logic  



===20150430======  
修改了一下scheduler的回调方式, 并且保证不会因为使用错误而导致崩溃.  
现在schedulers_update只返回一个SID了 这个作为这个任务的唯一ID. 停止只需要提供这个ID就可以了.  
不在对外提供Scheduler对象了. 因为很有可能你没有保存这个实例,导致出现的一些列问题.   
并且把Proc#call的方式修改为Method#call了  测试method方式比proc的方式快几乎1倍.  
所以就没有为了方便而提供接口了,当然 如果真的需要,也可以在脚本层做扩展   


===20150331======  
愚人节前夕,再更新一版.  
1.已经屏蔽掉了stream和package type这种通讯做法. 这样在项目变大的时候 会变得非常棘手(代码变庞大, 并且不好看啊啊啊啊   
2.全部通讯都使用一种叫做channel的通道来通讯, 会在本地与远程都建立一个对应的通道, 双方都可以在这个通道里.互相调用对方的函数.   就像调用本地函数一般.  
3.这样就不需要定义一大堆的pack_type了. 只剩下最基本的几个 比如 create_channel destory_channel call_channel return_channel  
4.增加一个广播的demo  其实基础流程就是  
node1 <-> center  
node2 <-> center  
node1 call center#broadcast( method_name, params )  
center -> (node1 node2)#method_name(params)  
唔..还是挺不错的 : P  
去掉了原先使用method_missing的方法来做RPC. 发现这样一旦项目大了后.   
会很难跟踪问题. 直接使用一个define_rpc函数来定义    
详细见 simple_dist  这是一个 多网关 多逻辑节点 单一中心服务器的demo  
其实一开始我是希望做成work分发的功能的. 但是建立channel后. 原先的意义变小了, 之后考虑在包头上加一个定向路由功能.   
这个只是在原先的基础上做的扩展,package_type 还是可以用的. 互不影响  ：Ｐ

  
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

#fsnet
进入fsnet  
执行  
ruby extconf.rb  
make & make install  


#demo
进入scripts/ 
编辑server.rb
可以根据自己想看的demo取消注释. ：）

