require 'dist_server/server/remote_server'

require 'dist_server/util/log'

module Gate

	class LogicServerProxy < RemoteServer

		define_rpc(:register_gate)
		define_rpc(:service_message)
		define_rpc(:new_client)
		define_rpc(:lost_client)


		attr_reader :server_info

		def tick

		end

		def publicity_service_handler(server_info, services_name)

			@server_info = server_info

			for service_name in services_name

				self.server.bind_service_logic(service_name, self)

			end

			FSLogger.get_logger(self).info( "#{server_info.host}:#{server_info.port} publicity services -> %s", services_name )

		end
	end


end
