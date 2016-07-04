require 'dist_server/server/remote_server'

require 'dist_server/util/log'

module Center

	class LogicServerProxy < RemoteServer

		attr_accessor :live

		def initialize(*args)
			super
			@state = -1
		end
		def dead?
			return @state == 0
		end
		def wait
			@state = 0
		end
		def on_disconnect
			super
			FSLogger.get_logger(self).warn("logic disconnect !!")
		end


		def tick
			@state = 1
		end

		def register_service_handler(server_info, services_name)

			FSLogger.get_logger(self).info( "#{server_info.host}:#{server_info.port} register services -> %s", services_name )

		end


		define_rpc(:register_center)
		define_rpc(:ping)


	end


end
