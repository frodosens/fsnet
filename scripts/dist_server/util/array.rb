require 'dist_server/util/hash'
require 'dist_server/server/serial_object'

class Array
end

module IOType

	PARAMS_TYPE_INT = 0
	PARAMS_TYPE_FLOAT = 1
	PARAMS_TYPE_STRING = 2
	PARAMS_TYPE_ARY = 3
	PARAMS_TYPE_HASH = 4
	PARAMS_TYPE_BOOL = 5
	PARAMS_TYPE_INT64 = 6
	PARAMS_TYPE_NIL  = 7
	PARAMS_TYPE_SERIAL_OBJ  = 8

end

class FSInputStream

	def read_type_val
		type = self.read_byte
		case type

			when IOType::PARAMS_TYPE_INT
				return self.read_int32
			when IOType::PARAMS_TYPE_FLOAT
				return self.read_double
			when IOType::PARAMS_TYPE_STRING
				return self.read_string
			when IOType::PARAMS_TYPE_ARY
				return self.read_params_array
			when IOType::PARAMS_TYPE_HASH
				return self.read_hash
			when IOType::PARAMS_TYPE_BOOL
				return self.read_bool
			when IOType::PARAMS_TYPE_NIL
				return nil
			when IOType::PARAMS_TYPE_INT64
				return self.read_int64
			when IOType::PARAMS_TYPE_SERIAL_OBJ
				return SerialObject._from_serial(self.read_string)
			else
				raise("Type match miss #{type}")
		end
	end

	def read_params_array

		ary = []
		size = self.read_uint16
		for i in 0...size
			ary << self.read_type_val
		end

		return ary
	end

end

class FSOutputStream

	def write_type_val(val)
		if val.is_a?(Array)
			self.write_byte(IOType::PARAMS_TYPE_ARY)
			self.write_params_array(val)
		elsif val.is_a?(Integer)
			self.write_byte(IOType::PARAMS_TYPE_INT)
			self.write_int32(val)
		elsif val.is_a?(Bignum)
			self.write_byte(IOType::PARAMS_TYPE_INT64)
			self.write_int64(val)
		elsif val.is_a?(Float)
			self.write_byte(IOType::PARAMS_TYPE_FLOAT)
			self.write_double(val)
		elsif val.is_a?(String) or val.is_a?(Symbol)
			self.write_byte(IOType::PARAMS_TYPE_STRING)
			self.write_string(val.to_s)
		elsif val.is_a?(Hash)
			self.write_byte(IOType::PARAMS_TYPE_HASH)
			self.write_hash(val)
		elsif val.is_a?(TrueClass)
			self.write_byte(IOType::PARAMS_TYPE_BOOL)
			self.write_bool(true)
		elsif val.is_a?(FalseClass)
			self.write_byte(IOType::PARAMS_TYPE_BOOL)
			self.write_bool(false)
		elsif val.nil?
			self.write_byte(IOType::PARAMS_TYPE_NIL)
		else
			if val.respond_to?(:_serial)
				self.write_byte(IOType::PARAMS_TYPE_SERIAL_OBJ)
				self.write_string(val._serial)
			else
				raise("Type match miss #{val.class}")
			end
		end
	end

	def write_params_array(ary)
		self.write_uint16(ary.size)
		ary.each do |val|
			write_type_val(val)
		end


	end

end