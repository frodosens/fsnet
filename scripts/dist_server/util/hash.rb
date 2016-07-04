require 'dist_server/util/array'

class Hash


end


class FSInputStream


	def read_hash
		size = self.read_uint16
		hash = {}
		for i in 0...size
			key = self.read_string
			value = self.read_type_val
			hash[key] = value
		end
		hash

	end
end

class FSOutputStream


	def write_hash(hash)
		self.write_uint16(hash.size)
		for k, v in hash
			self.write_string(k.to_s)
			self.write_type_val(v)
		end
	end

end