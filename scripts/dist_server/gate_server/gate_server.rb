require 'dist_server/server/base_server'
require 'dist_server/util/log'

require 'dist_server/gate_server/logic_server_proxy'
require 'dist_server/gate_server/client_proxy'


class GateServer < BaseServer

	def initialize(server_info)
		super(server_info)

		@service_map_logic = {} # { service_name => [] }

	end

	def on_create_new_client
		Gate::ClientProxy
	end

	def on_start
		super
		FSLogger.get_logger(self).info "gate server starting"

		self.connect_logic_servers
	end

	def get_server_with_service_name(service_name)
		logic_servers = @service_map_logic[service_name]
		if logic_servers.nil?
			return nil
		end
		return logic_servers[rand(logic_servers.size)]
	end

	def get_service_list
		@service_map_logic.keys
	end

	def bind_service_logic(service_name, logic)
		@service_map_logic[service_name] ||= []
		@service_map_logic[service_name] << logic
	end

	def connect_logic_servers
		@logic_servers = []
		for info in $server_boost.get_infos_with_type('logic')
			logic_proxy = Gate::LogicServerProxy.new self, info.host, info.port
			if logic_proxy.active && self.init_logic(logic_proxy)
				@logic_servers << logic_proxy
			end
		end
	end

	def init_logic(logic)
		return logic.register_gate
	end


end