
require 'pack_type.rb'
require 'util/uuid.rb'
require 'util/hash.rb'
require 'entity/entity_system.rb'

class TCPClient


	include PackTypeDefine
	include EntitySystem

	alias :_tcp_client_initialize :initialize
	def initialize(*args)
		_tcp_client_initialize(*args)
		self.init_entities
	end


	def send_entity(entity)

		os = FSOutputStream.new
		os.write_entity(entity)
		pack = Pack.create(Pack.generate_serial, PACK_TYPE_CREATE_ENTITY, os)
		self.send_pack(pack)

	end

end
