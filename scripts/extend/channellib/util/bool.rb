

class FSInputStream


	def read_bool
		self.read_byte == 1
	end


end

class FSOutputStream

	def write_bool(v)
		self.write_byte(v ? 1 : 0)
	end

end
