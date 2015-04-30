
class DatabaseServer < ChannelServer

	class OwnerChannel < ChannelBase

		def on_create(data=nil)


		end

		def find_data_by( table_name, key )

			full_key = "#{table_name}_#{key}"

			rpc_return self.local_owner.database.get( full_key )

		end
		def find_table_by( table_name, key )

			full_key = "#{table_name}_#{key}"

			rpc_return self.local_owner.database.hgetall( full_key )

		end


	end

end
