
require 'dist_server/server/serial_object'

class ServiceInfo

	extend SerialObject
	include SerialObject

	attr_reader :sid
	attr_reader :name

	def initialize(sid, name)
		@sid = sid
		@name = name
	end


end
