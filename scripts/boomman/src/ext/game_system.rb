
#==========================================================================================
#   系统基类
# 	By Frodo	2014-06-16
#==========================================================================================
class GameSystemBase
	
	attr_reader :running
	attr_reader :name
	
	def initialize
		@name = self.class.name
	end
	
	def start(server)
		@running = true
	end
	
	def stop
		@running = false
	end
	
	def restart(server)
		self.stop()
		self.start(server)
	end
	
	
end