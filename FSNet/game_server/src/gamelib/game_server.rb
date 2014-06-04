require 'yaml'
require 'logger'
require 'tcp_server.rb'
require 'agent_client.rb'
require 'cmds/game_cmds.rb'
require 'pack/pack_type.rb'
require 'pack/pack.rb'

class ChildNode < TCPClient
	
	# 子服务器返回想处理的包
	def on_handle_regist_child_pack(node_id, pack)
		
		count = pack.input.read_int16
		for i in 0...count
			type = pack.input.read_int16
			# 0一下是系统协议,不可重载
			if(type > 0)
				self.server.childs_node_handle[type] = node_id
			end
		end
	end
	
	# 从子节点处理完后返回的包, 要在这发回给我的父节点
	def on_handle_child_node(node_id, agent_pack)
		
		delegate_id = agent_pack.input.read_int32
		pack = Pack.create_from_agent_pack( agent_pack );
		begin
			self.server.send_pack_to(delegate_id, pack);
		rescue
			pack = nil;
		end
	end
	
  def on_handle_pack(node_id, fs_pack)
		pack = Pack.parse(fs_pack);
		case pack.pack_type
		when PACK_TYPE_REGIST_CHILD_PACK_HANDLE
			on_handle_regist_child_pack(node_id, pack);
		when PACK_TYPE_AGENT
			on_handle_child_node(node_id, pack);
		else
			print "未知协议\n";
		end
  end
end



class GameServer < TCPServer
	
	include GameCMDS

  attr_reader :configure;
	attr_reader :handle_map_configure;
	attr_reader :logger;
	attr_reader :childs_node;						# hash	{ node_id=>client, node_id=>client }
	attr_reader :childs_node_handle;		# hash  { pack_type => [ node_id, node_id ] }
	attr_reader :agent_nodes;						# hahs  { agent_id => { agent_node } } 代理节点集

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
		
		@childs_node_handle = {}
		@childs_node = {}
		@agent_nodes = {}
  end
	
	def connect_node( node_name )
		
		# 查找配置中的子节点
		node_configure = @configure[node_name];
		
		info("Connecting to #{node_configure["name"]}");
		client = ChildNode.new(self, node_configure["addr_ip"], node_configure["addr_port"]);
		# 连接有效的话
		if(client.active)
			# 保存子节点
			@childs_node[client.id] = client
			info("Connecting to #{node_configure["name"]} successful");
			# 发送子节点确认包
			pack = Pack.create( 0, PACK_TYPE_AS_CHILD_NODE,  nil ); 
			client.send_pack(pack);
			
			return client;
		else
			err("Connecting to #{node_configure["name"]} fail");
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
			# 测试用!
			# os = FSOutputStream.new()				# 
# 			os.write_byte(1);
# 			os.write_byte(2);
# 			os.write_byte(3);	
# 			response = Pack.create( 1, 62, os );
# 			
# 			@array = []
# 			for i in 0...100
# 				client = TCPClient.new(server, "127.0.0.1", 50560);
# 				client.send_pack(response);	
# 				@array << client
# 			end
			on_start_complete();
		}
		
	end
	
	def method_missing(method_name, *arg, &block)
		error("丢失方法#{method_name}(arg.to_s)");	
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
	
	# 处理一个代理包
	def on_handle_agent(node, pack)
		
		agent_id = pack.read_data.read_int32(); 
		
		agent_node = AgentNode.new(self, node, agent_id); 
		pack = Pack.create_from_is( pack.read_data );
		handle_pack(agent_node, pack);
		agent_node = nil;
		
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
			warn("无法找到处理#{pack.pack_type}协议的方法");
		end
		
		
	end
	
	# 处理一个包
  def on_handle_pack(node_id, fs_pack)
		super(node_id, fs_pack);
		pack = Pack.parse(fs_pack);
		# 先找子节点是否可处理
		if(@childs_node_handle != nil)
			handle_node_id = @childs_node_handle[pack.pack_type]
			if(handle_node_id != nil)
				child_node = @childs_node[handle_node_id];
				if(child_node != nil)
					# 创建一个代理包,发送给子节点
					agent_pack = Pack.create_agent( node_id, pack );
					child_node.send_pack( agent_pack );
					return;
				end
			end
		end
		
		# 如果没有子节点可以处理的
		if(@handle_map_configure != nil)
			handle_pack(@clients[node_id] ,pack);
		end
    
  end

end
