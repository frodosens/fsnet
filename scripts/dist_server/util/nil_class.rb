
class FSInputStream


	def read_nil
		nil
	end


end

class FSOutputStream

	def write_nil(v)
		self.write_byte(v)
	end

end
