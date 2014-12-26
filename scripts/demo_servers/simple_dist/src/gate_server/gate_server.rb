require "game_server.rb"
require 'pack_type.rb'

class GateServer < GameServer

	def on_start_complete
		super
		@logic_nodes   = connect_nodes("node_servers")
	end

	def cmd_reconnect(sender, pack)

	end


	# 请求广播
	def cmd_broadcast(sender, pack)

		p 'gate cmd_broadcast'
		for client_id, client in @clients
			client.send_pack(pack)
		end

	end


end