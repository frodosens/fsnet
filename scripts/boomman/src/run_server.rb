

require 'cmds/boomman_pack_type.rb'
require "game_server.rb"

class RunServer < GameServer
	
	attr_reader :home_system
  
	def on_agent_node_shudown(agent_node)
		if(agent_node != nil and agent_node.pid != nil)
			$game_players.logout(agent_node.pid)
		end
	end
	
	
	
	def on_start_complete()
		
		super();
		@db_server   = connect_node("db_server");
		@systems = []
		start_systems();
	
	
	end
	
	
	def start_systems
		
		$game_homes = GameHomeSystem.new
		$game_gems  = GameGemSystem.new
		
		@systems << $game_homes
		@systems << $game_gems
		
		begin
		
			for sys in @systems
				info("=== START #{sys.name} ===")
				sys.start(self)
			end
		
		rescue => e
			err(e.message)
		end
		
		
		
	end
	
  
end

