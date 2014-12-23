require 'util/hash.rb'


class Array
end

module IOType

	PARAMS_TYPE_INT = 0
	PARAMS_TYPE_FLOAT = 1
	PARAMS_TYPE_STRING = 2
	PARAMS_TYPE_ARY = 3
	PARAMS_TYPE_HASH = 4

end

class FSInputStream

	def read_type_val
		type = self.read_byte
		case type

			when IOType::PARAMS_TYPE_INT
				return self.read_int32
			when IOType::PARAMS_TYPE_FLOAT
				return self.read_float
			when IOType::PARAMS_TYPE_STRING
				return self.read_string
			when IOType::PARAMS_TYPE_ARY
				return self.read_params_array
			when IOType::PARAMS_TYPE_HASH
				return self.read_hash

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
		elsif val.is_a?(Float)
			self.write_byte(IOType::PARAMS_TYPE_FLOAT)
			self.write_float(val)
		elsif val.is_a?(String)
			self.write_byte(IOType::PARAMS_TYPE_STRING)
			self.write_string(val)
		elsif val.is_a?(Hash)
			self.write_byte(IOType::PARAMS_TYPE_HASH)
			self.write_hash(val)
		else
			raise("Type match miss #{val.class}")
		end
	end

	def write_params_array(ary)
		self.write_uint16(ary.size)
		ary.each do |val|
			write_type_val(val)
		end


	end

end