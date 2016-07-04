

require 'dist_server/server/rpc_interface'
require 'dist_server/util/log'


module Logic

	class GateServerProxy < RPCInterface

		# 存活确认
		define_rpc(:tick)

		define_rpc(:publicity_service_handler)

		def new_client(client_id)

			GC.start
			FSLogger.get_logger(self).info("has new connect id => %s", client_id)

		end

		def lost_client(client_id)

			FSLogger.get_logger(self).info("lost connect id => %s", client_id)

		end

		def service_message(client_id, service_name, method_name, *args)

			FSLogger.get_logger(self).info("#{client_id} call service #{service_name}##{method_name}(#{args})")

		end


	end

end
