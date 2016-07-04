require 'rubylib/tcp_server'
require 'dist_server/server/message'
require 'dist_server/util/log'


class BaseServer < GameTCPServer


	def initialize(server_info)

		@server_info = server_info
		super(server_info.name.nil? ? self.class.name : server_info.name)

	end

	def get_server_info
		@server_info
	end


	def get_sender
		Fiber.current.instance_variable_get('@sender')
	end

	def on_message(sender, message)

		if self.respond_to?(message.method)
			Fiber.current.instance_variable_set('@sender', sender)
			ret = self.send(message.method, *message.params)

			if message.need_return?

				ret_msg = Message.create_return_pack(message, ret)
				sender.send_pack(ret_msg.to_fs_pack)

			end

		else

			FSLogger.get_logger(self).warn("%s could not found msg %s", self.name, message.method)

		end

	end

	def wrap_client(node, klass)

	end


	def on_handle_pack(node_id, pack)
		super

		message = Message.create_from_pack(pack)

		if message.is_return_package?

			callback_id = message.get_callback_id
			params = message.get_return_params
			@clients[node_id].handle_return(callback_id, params)

		else

			if !@clients[node_id].on_message(@clients[node_id], message)
				self.on_message( @clients[node_id], message)
			end
		end

	end

end