
require 'game/modules/systems/aio_system.rb'

class GameModule


	attr_reader :server
	attr_reader :systems
	attr_reader :systems_name
	def initialize(server)
		@server = server
		@systems = {}
		@systems_name = []
		self.init_systems

	end

	def init_systems

		add_system(AIOSystem)

	end

	def get_system(system)
		@systems[system.name]
	end

	def shutdown_systems
		remove_system(AIOSystem)
	end

	def add_system(system)
		@systems[system.name] = system.new
		@systems_name << system.name
	end
	def remove_system(system)
		if @systems[system.name].nil?
			raise
		end
		@systems[system.class.name].shutdown
		@systems.delete(system.name)
		@systems_name.delete(system.name)
	end

	def update(dt)

		for system_name in @systems_name

			@systems[system_name].update(dt)

		end

	end

	def shutdown

	end


end