




class FSInputStream

	def read_pack

		pack = Pack.new(0)
		pack.init_from_is(self)
		data = self.read_data(pack.data_len)
		nis = FSInputStream.new( data, pack.data_len , false)
		pack.input = nis
		pack

	end


end

class FSOutputStream

	def write_pack(pack)

		if pack.read_data.nil?
			self.write_data( pack.write_data.data, pack.write_data.len );
		else
			self.write_data( pack.read_data.data, pack.read_data.len );
		end

	end

end
