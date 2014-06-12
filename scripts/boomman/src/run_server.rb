

require 'cmds/boomman_pack_type.rb'
require "game_server.rb"

class RunServer < GameServer
  
	def on_agent_node_shudown(agent_node)
		if(agent_node != nil and agent_node.pid != nil)
			$game_players.logout(agent_node.pid)
		end
	end
	

	def on_start_complete()
		
		super();
		
		@db_server   = connect_node("db_server");
		
	end
	
  
end

