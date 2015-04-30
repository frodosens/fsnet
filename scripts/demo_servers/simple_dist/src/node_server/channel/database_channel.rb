
class NodeServer < ChannelServer

	class DatabaseChannel < ChannelBase

		def on_create(data=nil)


		end



		define_rpc(:find_data_by)
		define_rpc(:find_table_by)

	end

end
