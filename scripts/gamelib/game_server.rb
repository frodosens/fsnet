require 'yaml'
require 'logger'
require 'tcp_server.rb'
require 'agent_client.rb'
require 'cmds/game_cmds.rb'
require 'pack/pack_type.rb'
require 'pack/pack.rb'


class ChildNode < TCPClient
	
	attr_accessor :name
	attr_reader :pack_result_callback	# serial => proc
	attr_reader :pack_result_callback_data	# serial => data
	def initialize(*args)
		super(*args)
		@pack_result_callback = {}
		@pack_result_callback_data = {}
	end
	
	# 子服务器返回想处理的包
	def on_handle_regist_child_pack(node_id, pack)
		
		count = pack.input.read_int16
		for i in 0...count
			type = pack.input.read_int16
			# 0以下是系统协议,不可重载
			if(type > 0)
				self.server.childs_node_handle[type] = node_id
			end
			
		end
		
	end
	
	# 子服务返回处理完毕的包
	def on_handle_child_node(node_id, pack)
		# 执行返回的回调
		if(@pack_result_callback[pack.serial] != nil)
			@pack_result_callback[pack.serial].call(pack, @pack_result_callback_data[pack.serial])
			@pack_result_callback.delete(pack.serial)
			@pack_result_callback_data.delete(pack.serial)
		else
		
			delegate_id = pack.input.read_int32
			result_pack = Pack.create_from_agent_pack( pack );
			server.send_pack_to(delegate_id, result_pack);
			
			# print "#{server.name} #{pack.pack_type} is not callback \n"
		end
	end
	
	# 向子节点发送一个包
	# pack : Pack
	# data : 用户数据
	# callback
	def send_pack(pack, data=nil, callback=nil)
		if(callback != nil)
			@pack_result_callback[pack.serial] = callback
		end
		if(data != nil)
			@pack_result_callback_data[pack.serial] = data
		end
		super(pack)
	end
	
	# 主动发送的
	def on_handle_know_pack(node_id, pack)
		p "on_handle_know_pack#{pack.type}"
	end
	
  def on_handle_pack(node_id, fs_pack)
		pack = Pack.parse(fs_pack);
		case pack.pack_type
		when PACK_TYPE_REGIST_CHILD_PACK_HANDLE
			on_handle_regist_child_pack(node_id, pack);
		when PACK_TYPE_AGENT
			on_handle_child_node(node_id, pack);
		else
			on_handle_know_pack(node_id, pack);
		end
  end
end


class GameServer < GameTCPServer
	
	include GameCMDS

  attr_reader :configure;
	attr_reader :handle_map_configure;
	attr_reader :logger;
	attr_reader :childs_node;						# hash	{ node_id=>client, node_id=>client }
	attr_reader :childs_node_handle;		# hash  { pack_type => [ node_id, node_id ] }
	attr_reader :agent_nodes;						# hahs  { agent_id => { agent_node } } 代理节点集
	attr_reader :agent_nodes;

  def initialize(configure_file_name)
    begin
    	configure_file = File.open(configure_file_name);
    	@configure = YAML.load(configure_file);
		rescue => err
			print(err.message + "\n");
		ensure
    	configure_file.close();
		end
		
	
    super(configure["base_configure"]["server_name"]);
		
		if(configure["base_configure"]["byte_order"] != nil)
    	@byte_order = configure["base_configure"]["byte_order"];
		end
		
		if(configure["base_configure"]["logger_file"] != nil)
			@logger_file = Logger.new(configure["base_configure"]["logger_file"]);
		else
			@logger_file = Logger.new(configure["server_name"] + "log");
		end
		
		if(configure["pack_handle"] == nil)
			err("找不到协议映射函数表");
			@handle_map_configure = {};
		else
			type_map = configure["pack_handle"];
			@handle_map_configure = {};#configure["pack_handle"];
			
			for key, value in type_map
				itype = key;
				if(key.class == String)
					itype = eval( key );
				end
				@handle_map_configure[itype] = value
			end
		end
		
		# 系统协议,重定义
		@handle_map_configure[PACK_TYPE_AS_CHILD_NODE] = "on_as_child_node"
		@handle_map_configure[PACK_TYPE_REGIST_CHILD_PACK_HANDLE] = "on_regist_child_handle_pack"
		@handle_map_configure[PACK_TYPE_AGENT] = "on_handle_agent"
		@handle_map_configure[PACK_AGENT_NODE_SHUDOWN] = "on_handle_agent_shudown"
		
		@childs_node_handle = {}
		@childs_node = {}
		@agent_nodes = {}
  end
	
	def find_node_by_name( node_name )
	
		for node_id, node in @childs_node
		
			if(node.name == node_name)
				return node
			end
		
		end
		
		return nil
		
	end
	
	def connect_node( node_name )
		
		# 查找配置中的子节点
		node_configure = @configure[node_name];
		
		if(node_configure.nil?)
			warn(" #{node_name} is not defined ");
			return nil
		end
		
		info("#{self.name} Connecting to #{node_configure["name"]}");
		client = ChildNode.new(self, node_configure["addr_ip"], node_configure["addr_port"]);
		# 连接有效的话
		if(client.active)
			# 保存子节点
			@childs_node[client.id] = client
			client.name = node_name
			info("#{self.name} Connecting to #{node_configure["name"]} successful");
			# 发送子节点确认包
			pack = Pack.create( 0, PACK_TYPE_AS_CHILD_NODE,  nil ); 
			client.send_pack(pack);
			
			return client;
		else
			err("#{self.name} Connecting to #{node_configure["name"]} fail");
		end
		
		return nil;
	end
	
	def warn(log)
		@logger_file.warn(log);
		print log, "\n"
	end
	
	def err(log)
		@logger_file.error(log);
		print log, "\n"
		if($@ != nil)
			for ermsg in $@
				@logger_file.error(ermsg);
				print ermsg,"\n"
			end
		end
	end
	
	def info(log)
		@logger_file.info(log)
		print log, "\n"
	end
  
  def start()
		
		start_server(@configure["base_configure"]["addr_ip"], @configure["base_configure"]["addr_port"].to_i){ |server|
			on_start_complete();
		}
		
	end
	
	def method_missing(method_name, *arg, &block)
		err("丢失方法 #{method_name} (#{arg.to_s})");	
	end
	
	
	def on_start_complete()
		info("#{self.name}[ #{@configure["base_configure"]["addr_ip"]}, #{@configure["base_configure"]["addr_port"].to_i.to_i} ]启动完成");
	end
	
	# 被确认是子节点
	# 此时我应该返回一个PACK_TYPE_REGIST_CHILD_PACK_HANDLE包
	def on_as_child_node(parent, pack)
		
		# 发送我可以执行的协议
		os = FSOutputStream.new();
		os.write_int16(@handle_map_configure.length);
		for key, value in @handle_map_configure
			os.write_int16(key);
		end
		
		
		response = Pack.create( pack.serial, PACK_TYPE_REGIST_CHILD_PACK_HANDLE, os );
		parent.send_pack(response);
		
	end
	
	# 当代理节点断开时
	def on_agent_node_shudown(agent_node)
		
	end
	
	# 处理一个代理节点断开
	def on_handle_agent_shudown(node, pack)
		
		agent_id = pack.read_data.read_int32(); 
		on_agent_node_shudown(@agent_nodes[agent_id])
		@agent_nodes.delete(agent_id);
		
		# 通知子节点,我的父节点断开了
		os = FSOutputStream.new()
		os.write_uint32(agent_id)
		pack = Pack.create( 0, PACK_AGENT_NODE_SHUDOWN , os )
		for node in @childs_node.values
			node.send_pack(pack)
		end
		
	end
	
	# 处理一个代理包
	def on_handle_agent(node, pack)

		# 读出代理ID
		agent_id = pack.read_data.read_int32();
		
		# 建立代理节点 
		agent_node = @agent_nodes[agent_id];
		if(agent_node.nil?)
			agent_node = AgentNode.new(self, node, agent_id);
			@agent_nodes[agent_id] = agent_node;
		end
		
		# 上一级发过来的序列号
		serial = pack.serial;
		
		pack = Pack.create_from_is( pack.read_data );
		# 非根节点转发,不能自己生成,要统一一个序列号,不然无法返回到根节点
		if(!child_handle_pack(node.id, pack, agent_id, serial, @@node_result_callback))
			# 设置正在代理的序列号
			agent_node.agent_serial = serial
			# 让代理节点处理
			handle_pack(agent_node, pack);
		end
		
	end
	

	# 当有连接关闭的时候
  def on_shudown_node(node_id)
		super(node_id)
		
		# 通知子节点,我的父节点断开了
		os = FSOutputStream.new()
		os.write_uint32(node_id)
		pack = Pack.create( 0, PACK_AGENT_NODE_SHUDOWN , os )
		for node in @childs_node.values
			node.send_pack(pack)
		end
		
  end
	
	
	
	# 本地处理一个包
	def handle_pack( client, pack )
		method_name = @handle_map_configure[pack.pack_type];
		if(method_name != nil)
			begin
				pack_method = method(method_name);
				if(pack_method != nil)
					pack_method.call(client ,pack);
				end
			rescue => e
				err(e.message);
			end
		else
			warn("#{name} 无法找到处理#{pack.pack_type}协议的方法");
		end
		
	end
	

	@@root_result_callback = proc { |result, data| 
	
		server = data[0]
		nid    = data[1]
		
		delegate_id = result.input.read_int32
		result_pack = Pack.create_from_agent_pack( result );
		begin
			server.send_pack_to(nid, result_pack)
		rescue => err
		end
	
	}

	@@node_result_callback = proc{ |result, data|
		
		server = data[0]
		nid    = data[1]
		server.send_pack_to(nid, result)
		
	}
	
	# 尝试让子节点处理一个包
	# node_id : 真实节点ID
	# pack : Pack
	# agent_id : 如果自己本身就是代理, 二级代理的代理ID
	# 指定二级代理的序列号
	def child_handle_pack(node_id, pack, agent_id=nil, serial=nil, call_back_proc=@@root_result_callback)
	
		# 先找子节点是否可处理
		if(@childs_node_handle != nil)
			handle_node_id = @childs_node_handle[pack.pack_type]
			if(handle_node_id != nil)
				child_node = @childs_node[handle_node_id];
				if(child_node != nil)
					# 创建一个代理包,发送给子节点, 处理完后,再发回父节点
					
					agent_pack = Pack.create_agent( agent_id.nil? ? node_id : agent_id, pack , serial);
					
					child_node.send_pack( agent_pack , [self, node_id] ,call_back_proc) 
					
					# print "#{name} send to #{child_node.name} (#{agent_pack.serial}) \n"
					
					return true;
				end
			end
		end
	
		return false;
	
	end
	
	# 处理一个包
  def on_handle_pack(node_id, fs_pack)
		super(node_id, fs_pack);
		pack = Pack.parse(fs_pack);
		
		# 先找子节点是否可处理
		if(child_handle_pack(node_id, pack))
			return
		end
		
		# 如果没有子节点可以处理的
		if(@handle_map_configure != nil)
			handle_pack(@clients[node_id] ,pack);
		end
    
  end

end
