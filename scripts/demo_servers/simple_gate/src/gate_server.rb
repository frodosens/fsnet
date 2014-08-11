require "game_server.rb"



class GateServer < GameServer
	
	def on_start_complete
		super()

    @simple_server   = connect_node("simple_server")
    @time_server   = connect_node("time_server")
		
	end
	
end
