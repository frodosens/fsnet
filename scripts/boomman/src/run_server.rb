

require 'cmds/boomman_pack_type.rb'
require "game_server.rb"

class RunServer < GameServer
  
	# 当根节点出现断开连接时候的通知
	# 可以在这里做player登出的操作,或者其他
	def on_agent_node_shudown(agent_node)
		
	end
	
	
	
	def on_start_complete()
		
		super();
		@db_server   = connect_node("db_server");
	
	end
	
	def cmd_version(sender, pack)

		max_version  = pack.input.read_byte();		# 读出3个版本号
		mind_version = pack.input.read_byte();
		min_version  = pack.input.read_byte();
		
		os = FSOutputStream.new()				# 
		
		os.write_byte(max_version);			# 写回给客户端
		os.write_byte(mind_version);
		os.write_byte(min_version);
		
	  response = Pack.create( pack.serial, PACK_TYPE_VERSION, os );
		
	  sender.send_pack(response);
		
	end
		
  
end

