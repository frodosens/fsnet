
class NodeServer < ChannelServer

	class GateChannel < ChannelBase

		def on_create(data=nil)



		end

		def test(a,b,c,d,e)
            #on_test_repson(a,b,c,d,e)
			
		end

		define_rpc(:on_test_repson)
		
	end

end