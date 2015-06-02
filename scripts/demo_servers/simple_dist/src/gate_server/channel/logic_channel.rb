
class GateServer < ChannelServer


	class LogicChannel < ChannelBase

		def on_create(data=nil)
			return data
		end

		def on_test_repson(a,b,c,d,e)
			test(a,b,c,d,e)
		end

		define_rpc(:test)

	end


end