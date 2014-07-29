require 'cmds/boomman_pack_type.rb'


require "game_server.rb"

class GateServer < GameServer

	attr_reader :login_server;
	attr_reader :run_server;
	def on_start_complete()
		
		super();
		
		@login_server = connect_node("login_server");
		@run_server   = connect_node("run_server");
		
	end
	
	#
	def cmd_regiest_session(sender, pack)
		
		done = 1
		# 在这里做点session初始化的事情
		os = FSOutputStream.new()				# 
		os.write_byte(done);
		
		# 发回给客户端
	  response = Pack.create( pack.serial, PACK_TYPE_REGIST_SESSION, os );
	  sender.send_pack(response);
		
	end
	

end
