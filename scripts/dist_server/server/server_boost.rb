

require 'rexml/document'

require 'dist_server/logic_server/logic_server'
require 'dist_server/gate_server/gate_server'
require 'dist_server/center_server/center_server'
require 'dist_server/server/server_info'
require 'dist_server/util/log'


class ServerBoot

	def initialize(conf)

		@conf = conf
		@servers_info_list = {}
		@servers_instances = {}
		self.parse_server_list

	end

	def get_infos_with_type(type)
		return @servers_info_list[type]
	end

	def start(*servers_type)

		for type in servers_type
			self.start_server_with_type type
		end

	end

	def start_server_with_type(type)
		for server_info in @servers_info_list[type]
			create_server_with_info server_info
		end
	end

	def create_server_with_info(info)

		klass = Object.const_get(info.klass)
		if klass.nil?
			FSLogger.get_logger(this).error "could not found server class %s", info.klass
			return
		end

		server = klass.new info
		server.start_server info.host, info.port
		@servers_instances[info.type] ||= []
		@servers_instances[info.type] << server

	end

	def parse_server_list

		File.open(@conf) do |file|
			document = REXML::Document.new file.read
			for server_list in document.root.elements
				for server in server_list.elements

					server_info = ServerInfo.new server.attributes['name'],
												 server_list.name,
					               server.attributes['type'],
					               server.attributes['host'],
					               server.attributes['port'].to_i,
					               server.attributes['services'].nil? ? [] : server.attributes['services'].split(/[,;]/)

					@servers_info_list[server_list.name] ||= []
					@servers_info_list[server_list.name] << server_info

				end
			end

		end

	end

end
