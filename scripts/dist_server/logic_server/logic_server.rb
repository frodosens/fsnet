require 'dist_server/server/base_server'
require 'dist_server/util/log'
require 'dist_server/logic_server/center_server_proxy'
require 'dist_server/logic_server/gate_server_proxy'

class LogicServer < BaseServer

	def initialize(server_info)
		super(server_info)

		@center_server = nil
		@gate_list = []
		@services = server_info.services


	end

	# center 和 gate 会连接过来, 他们都是remote server
	def on_create_new_client
		RemoteServer
	end

	def on_start

		super
		FSLogger.get_logger(self).info "logic server starting"

		for service in self.get_server_info.services


			FSLogger.get_logger(self).info "create service #{service}"


		end

		self.scheduler_update(1 / 1000, -1, :tick_center)

	end


	def register_gate
		gate = Logic::GateServerProxy.new get_sender
		gate.publicity_service_handler(self.get_server_info, @services)
		@gate_list << gate
		FSLogger.get_logger(self).info "gate register succ"
	end

	def register_center
		@center_server = Logic::CenterServerProxy.new get_sender
		@center_server.register_service_handler(self.get_server_info, @services)
		FSLogger.get_logger(self).info "center register succ"
		true
	end


	def tick_center(sid, dt)
		if @center_server
			@center_server.tick_sync
		end
		for gate in @gate_list
			gate.tick_sync
		end
	end

end