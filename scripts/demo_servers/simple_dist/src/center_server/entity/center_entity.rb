require 'entity/entity_base.rb'


class CenterServer < GameServer


	class CenterEntity < EntityBase


		def on_create(data=nil)

		end

		define_rpc(:hello, String)
		def hello(name)
			self.hello_reply("I'm CenterServer this reply")
		end


		define_rpc(:hello_reply, String)



	end

end