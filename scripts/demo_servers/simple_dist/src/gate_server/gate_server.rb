require "game_server.rb"
require 'pack_type.rb'
require "gate_server/channel/logic_channel.rb"

# ======
#   一般来讲 这Gate是最根的节点
#
#

class GateServer < ChannelServer

	attr_reader :group_map  # {  }


	def on_start_complete
		super

		self.init_logic_node


	end

	def init_logic_node

		@logic_nodes   = connect_nodes("node_servers")
		self.init_logic_channel

	end

	def init_logic_channel

		for node in @logic_nodes

			channel = node.create_channel(GateServer::LogicChannel, "NodeServer::GateChannel", self)

			node.send_channel(channel)

		end

	end


	# ====
	# 如果有子集群可以执行的情况下, 交给选择器选择
	#
	def get_agent_node_by_group(sender_id, group, pack_type)
		group[ sender_id %  group.size]
	end



end