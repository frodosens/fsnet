

class A

end

class DatabaseServer < ChannelServer

	class OwnerChannel < ChannelBase

		def on_create(data=nil)


		end

		def find_data_by( table_name, key )
			rpc_return ({  })
		end


	end

end
