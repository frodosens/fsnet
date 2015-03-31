
require 'pack_type.rb'
require 'util/array.rb'

class FSOutputStream


	def write_channel(channel)

		self.write_string(channel.uuid.to_s)
		self.write_string(channel.class.to_s)
		self.write_string(channel.remote_klass_name)
		self.write_hash(channel.on_create_data.nil? ? {} : channel.on_create_data)

	end

end

class FSInputStream

	def read_channel



	end

end

class ChannelBase

	include PackTypeDefine

	attr_accessor :uuid
	attr_accessor :remote_klass_name
	attr_accessor :owner
	attr_accessor :local_owner
	attr_accessor :calling_serial
	attr_accessor :on_create_data

	def initialize(  )
		@uuid = nil
		@remote_class_name = self.class.to_s
		@owner = nil
		@local_owner = nil
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
	#
	# def method_missing(method_name, *arg, &block)
	#
	# 	return  super if( @@_rpc_define.nil? or @@_rpc_define[self.class.to_s].nil? )
	#
	# 	unless @@_rpc_define[self.class.to_s].include?(method_name)
	# 		return super
	# 	end
	#
	# 	self.instance_eval(
	# 		"def #{method_name}(*arg, &block)
	# 				unless @owner.nil?
	# 					os = FSOutputStream.new
	# 					os.write_string(self.uuid.to_s)
	# 					os.write_string('#{method_name}')
	# 					os.write_params_array(arg)
	# 					os.write_byte( block.nil? ? 0 : 1)  # 是否要求返回
	# 					pack = Pack.create(Pack.generate_serial, PACK_TYPE_MESSAGE_CHANNEL, os)
	# 					if block.nil?
	# 						@owner.send_pack(pack)
	# 					else
	# 						@owner.send_pack(pack, block, @@_rpc_call_return)
	# 					end
	# 				end
	# 		 end"
	# 	)
	# 	self.send(method_name, *arg, &block)
	#
	# end


	class << self

		def define_rpc(method_name, *args)

			#@@_rpc_define ||= {}
			#@@_rpc_define[self.to_s] ||= {}
			#@@_rpc_define[self.to_s][method_name] = args

			class_eval("def #{method_name}(*arg, &block)
					unless @owner.nil?
						os = FSOutputStream.new
						os.write_string(self.uuid.to_s)
						os.write_string('#{method_name}')
						os.write_params_array(arg)
						os.write_byte( block.nil? ? 0 : 1)  # 是否要求返回
						pack = Pack.create(Pack.generate_serial, PACK_TYPE_MESSAGE_CHANNEL, os)
						if block.nil?
							@owner.send_pack(pack)
						else
							@owner.send_pack(pack, block, @@_rpc_call_return)
						end
					end
			 end", __FILE__, __LINE__)


		end

		def create(uuid=nil)

			channel = self.new
			channel.uuid = uuid
			channel.uuid = UUID.generate if uuid.nil?
			channel

		end


	end

end