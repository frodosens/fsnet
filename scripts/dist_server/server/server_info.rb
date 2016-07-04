
require 'dist_server/server/serial_object'

class ServerInfo

	extend SerialObject
	include SerialObject

	attr_reader :name
	attr_reader :type
	attr_reader :klass
	attr_reader :host
	attr_reader :port
	attr_reader :services

	def initialize(name, type, klass, host, port, services)
		@name = name
		@type = type
		@klass = klass
		@host = host
		@port = port
		@services = services
	end


end
