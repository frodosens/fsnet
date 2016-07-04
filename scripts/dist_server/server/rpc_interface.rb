require 'dist_server/server/rpc_define'

class RPCInterface

	extend RPCDefine

	def initialize(node)
		@agent_node = node
		@agent_node.set_message_handler(self)
	end

	def send_message_async(*args)
		@agent_node.send_message_async(*args)
	end

	def send_message_sync(*args)
		@agent_node.send_message_sync(*args)
	end


end