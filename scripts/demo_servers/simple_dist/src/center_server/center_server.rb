
require 'pack_type.rb'
require "game_server.rb"
require 'util/uuid.rb'
require 'util/hash.rb'
require "channel/channel_server.rb"
require 'center_server/channel/center_channel.rb'


class CenterServer < ChannelServer

	attr_reader :nodes
	attr_reader :online_count

	def initialize(*args)
		super
		@nodes = {}
		@online_count = 0
		self.init_entities
	end

	def on_start_complete
		super

	end

	def on_login(pid)
		@online_count += 1
	end

	def on_logout(pid)
		@online_count -= 1
	end

	def broadcast( method_name, *params )


		for node in @nodes.values
			channel = node.find_channel(NodeChannel)

			channel.send(method_name, *params)
		end

	end


	def regist_node(node_name , sender)
		if @nodes.include?(node_name)
			false
		else
			@nodes[node_name] = sender
			info("cmd_regist_node #{node_name}")
			true
		end
	end


end