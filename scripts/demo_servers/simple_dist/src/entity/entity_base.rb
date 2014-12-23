
require 'pack_type.rb'
require 'util/array.rb'

class FSOutputStream


	def write_entity(entity)

		self.write_string(entity.uuid.to_s)
		self.write_string(entity.class.to_s)
		self.write_string(entity.remote_klass_name)
		self.write_hash({})

	end

end

class FSInputStream

	def read_entity

	end

end

class EntityBase

	include PackTypeDefine

	attr_accessor :uuid
	attr_accessor :remote_klass_name
	attr_accessor :owner
	attr_accessor :calling_serial

	def initialize(  )
		@uuid = nil
		@remote_class_name = self.class.to_s
		@owner = nil
		@calling_serial = -1
	end

	def on_create(data=nil)

	end

	def on_destroy

	end

	def rpc_return( data )

		return if self.calling_serial == -1

		os = FSOutputStream.new
		os.write_type_val(data)

		pack = Pack.create(self.calling_serial, PACK_TYPE_MESSAGE_RETURN, os)
		@owner.send_pack(pack)

	end

	@@_rpc_call_return = Proc.new { |ret_pack, ret_block|
		val = ret_pack.input.read_type_val
		ret_block.call(val)
	}

	def method_missing(method_name, *arg, &block)

		self.instance_eval(
			"def #{method_name}(*arg, &block)
					unless @owner.nil?
						os = FSOutputStream.new
						os.write_string(self.uuid.to_s)
						os.write_string('#{method_name}')
						os.write_params_array(arg)
						os.write_byte( block.nil? ? 0 : 1)  # 是否要求返回
						pack = Pack.create(Pack.generate_serial, PACK_TYPE_MESSAGE_ENTITY, os)
						if block.nil?
							@owner.send_pack(pack)
						else
							@owner.send_pack(pack, block, @@_rpc_call_return)
						end
					end
			 end"
		)
		self.send(method_name, arg, &block)

	end


	class << self

		def define_rpc(method_name, *args)

			@@_rpc_define ||= {}

			@@_rpc_define["#{self.to_s}#" + method_name.to_s] = args

		end

		def create(uuid=nil)

			entity = self.new
			entity.uuid = uuid
			entity.uuid = UUID.generate if uuid.nil?
			entity

		end


	end

end