require 'singleton'
require 'fiber'

require 'dist_server/server/rpc_define'
require 'rubylib/tcp_client'

class LoopIdGenerator
	include Singleton

	def initialize
		@id = 0
	end

	def next_id

		if @id >= 0x7fffffff
			@id = 0
		end

		@id += 1

	end
end

class RemoteServer < TCPClient

	extend RPCDefine

	def initialize(*args)
		super(*args)
		@fiber = {}
		@message_handler = self
	end

	def set_message_handler(handler)
		@message_handler = handler
	end

	def on_shutdown
		super
		on_disconnect
	end

	def on_disconnect
		super
		for id, fiber in @fiber
			fiber.resume
		end
		@fiber.clear
	end


	def on_handle_pack(sender, pack)
		message = Message.create_from_pack(pack)

		if message.is_return_package?
			callback_id = message.get_callback_id
			params = message.get_return_params
			self.handle_return(callback_id, params)
		else
			self.on_message(self, message)
		end

	end


	def on_message(sender, message)

		if @message_handler.respond_to?(message.method)

			Fiber.current.instance_variable_set('@sender', sender)
			ret = @message_handler.send(message.method, *message.params)

			if message.need_return?

				ret_msg = Message.create_return_pack(message, ret)
				sender.send_pack(ret_msg.to_fs_pack)

			end
			return true
		end
		false
	end

	def handle_return(callback_id, ret)
		fiber = @fiber[callback_id]
		if !fiber.nil?
			@fiber.delete callback_id
			fiber.resume ret
		end
	end

	def send_message_async(msg)
		id = LoopIdGenerator.instance.next_id
		msg.set_callback_id(id)
		if self.send_pack(msg.to_fs_pack)
			@fiber[id] = Fiber.current
			Fiber.yield
		end
	end

	def send_message_sync(msg)
		self.send_pack(msg.to_fs_pack)
	end

end