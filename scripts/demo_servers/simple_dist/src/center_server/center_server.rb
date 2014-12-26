
require 'pack_type.rb'
require "game_server.rb"
require 'util/uuid.rb'
require 'util/hash.rb'
require "channel/channel_server.rb"
require 'center_server/channel/center_channel.rb'


class CenterServer < ChannelServer

	attr_reader :nodes

	def initialize(*args)
		super
		@nodes = {}
		self.init_entities
	end

	def on_start_complete
		super

		self.create_server_channel
	end

	def create_server_channel


	end


	# 请求注册为一个逻辑节点
	def cmd_regist_node(sender, pack)

		node_name = pack.input.read_string

		@nodes[node_name] = sender

		info("cmd_regist_node #{name} : #{sender}")

		# 纯粹的回应成功
		sender.send_pack(pack)


	end
	# 请求广播到所有的逻辑节点
	def cmd_broadcast(sender, pack)

		for client_id, client in @clients
			client.send_pack(pack)
		end

	end

	# 请求调用指定一个逻辑节点的函数
	def cmd_call_node_rpc(sender, pack)
		# 调用那个一个节点
		node_name = pack.input.read_string
		if @nodes[node_name].nil?
			err("node is not exist")
			return;
		end

	end



end