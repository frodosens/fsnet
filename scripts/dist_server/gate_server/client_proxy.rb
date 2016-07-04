require 'dist_server/server/remote_server'

module Gate

	class ClientProxy < RemoteServer

		# 告诉客户端有哪些service可以用
		define_rpc(:publicity_services)

		def initialize(*args)
			super(*args)
			@client_id = nil
			@service_map = {}
		end

		def set_client_id(client_id)
			@client_id = self.server.get_server_info.name + "-" + client_id.to_s
		end


		def on_disconnect
			super

			self.find_service('PlayerService').lost_client(@client_id)

		end

		def find_service(service_name)
			@service_map[service_name]
		end

		def service_message(service_name, method_name, *args)
			service = self.find_service(service_name)
			if service
				service.service_message(@client_id, service_name, method_name, *args)
			end
		end

		def new_connect

			self.set_client_id(self.id)

			# 为每一个service选择一个server
			for service_name in self.server.get_service_list
				@service_map[service_name] = self.server.get_server_with_service_name(service_name)
			end

			self.find_service('PlayerService').new_client(@client_id)

			# 告诉客户端service_list, ! 远程客户端不要使用同步函数 !
			self.publicity_services_sync(*@service_map.keys)

		end

	end

end
