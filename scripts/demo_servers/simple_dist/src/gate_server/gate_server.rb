require "game_server.rb"
require 'pack_type.rb'

class GateServer < GameServer

	def on_start_complete
		super
		@logic_nodes   = connect_nodes("node_servers")
	end

end