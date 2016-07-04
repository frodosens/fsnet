

require 'dist_server/server/rpc_interface'



module Logic
	class CenterServerProxy < RPCInterface

		# 存活确认
		define_rpc(:tick)

		define_rpc(:register_service_handler)


	end
end
