
class NodeServer < ChannelServer


	class CenterChannel < ChannelBase

		def on_create(data=nil)


		end

		# 这里是测试广播聊天, 这里可以广播到自己所连接的客户端上
		def chat(message)

		end

		# RPC Method
		define_rpc(:regist_logic_node)
		define_rpc(:on_login)
		define_rpc(:get_online_count)
		define_rpc(:broadcast)
		define_rpc(:call_channel)
		define_rpc(:find_channel_by_pid)



	end


end