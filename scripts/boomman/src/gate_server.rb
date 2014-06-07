require 'cmds/boomman_pack_type.rb'


require "game_server.rb"

class GateServer < FSNET::GameServer

	attr_reader :login_server;
	attr_reader :run_server;
	def on_start_complete()
		
		super();
		
		@login_server = connect_node("login_server");
		@run_server   = connect_node("run_server");
		
	end
	
	

end
