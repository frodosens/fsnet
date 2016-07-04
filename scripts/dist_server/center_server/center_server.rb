require 'dist_server/server/base_server'
require 'dist_server/center_server/logic_server_proxy'
require 'dist_server/util/log'



class CenterServer < BaseServer

	def initialize(server_info)
		super(server_info)
		@logic_servers = []
		@global_services = {}
	end

	def on_start

		super

		FSLogger.get_logger(self).info 'center server starting'

		self.connect_logic_servers
		self.scheduler_update 5, -1, :check_logic_live

	end

	def connect_logic_servers
		@logic_servers = []
		for info in $server_boost.get_infos_with_type('logic')
			logic_proxy = Center::LogicServerProxy.new self, info.host, info.port
			if logic_proxy.active && self.init_logic(logic_proxy)
				@logic_servers << logic_proxy
			end
		end
	end

	def init_logic(logic)
		if logic.active
			logic.wait
			logic.register_center
			return true
		end
		false
	end

	def check_logic_live(*args)

		dead = []
		for logic in @logic_servers
			if logic.dead?
				self.on_logic_dead(logic)
				dead << logic
			else
				logic.wait
			end
		end
		for logic in dead
			logic.close
			@logic_servers.delete logic
		end
	end

	def on_logic_dead(logic_server)
		FSLogger.get_logger(self).warn ("logic dead #{logic_server}")
	end


end