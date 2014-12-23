require 'entity/entity_base.rb'

class NodeServer < GameServer


	class CenterEntity < EntityBase

		def on_create(data=nil)


		end

		# 远程方法定义
		define_rpc(:hello, String)

		# 定义本地方法
		define_rpc(:hello_reply, String)
		def hello_reply(name)
			self.hello(name)
		end


	end


end