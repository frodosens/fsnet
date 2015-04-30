require 'center_server/channel/center_channel.rb'


class CenterServer < ChannelServer

	attr_reader :nodes
	attr_reader :online_count
	attr_reader :online_map

	def initialize(*args)
		super
		@nodes = {}
		@online_map = {}
		@online_count = 0
	end

	def on_start_complete
		super

		@tick_id = scheduler_update(0.16, -1, :on_tick)
		@tick_id2 = scheduler_update(0.32, -1, :on_tick)

	end

	def on_tick( sid,  dt )
        p sid
	end

    
	def on_login( channel_name, pid)

		@online_map[pid] = channel_name
		@online_count += 1

	end

	def on_logout( channel_name, pid)

		@online_map.delete ( pid )
		@online_count -= 1

	end

	def broadcast( method_name, *params )

		for node in @nodes.values
			channel = node.find_channel(NodeChannel)

			channel.send(method_name, *params)
		end

	end

	def call_channel( channel_name, method_name, *params )

		for name, node in @nodes
			if name == channel_name
				channel = node.find_channel(NodeChannel)
				channel.send(method_name, *params)
			end
		end

	end

	def find_channel_by_pid( pid )

		@online_map[pid]

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