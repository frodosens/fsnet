

class CenterServer < ChannelServer


	class NodeChannel < ChannelBase


		def on_create(data=nil)

		end

		def broadcast( method_name, *params )
			self.local_owner.broadcast(method_name, *params)
		end

		def call_channel( channel_name, method_name, *params)
			self.local_owner.call_channel(channel_name, method_name, *params)
		end

		def find_channel_by_pid( pid )
			rpc_return self.local_owner.find_channel_by_pid( pid )
		end

		def get_online_count
			rpc_return self.local_owner.online_count
		end

		def on_login( channel_name, pid )
			self.local_owner.on_login( channel_name, pid )
		end

		def on_logout( channel_name, pid )
			self.local_owner.on_login( channel_name, pid )
		end

		def regist_logic_node(node_name)

			rpc_return self.local_owner.regist_node(node_name, self.owner)

		end


		define_rpc(:chat)



	end

end