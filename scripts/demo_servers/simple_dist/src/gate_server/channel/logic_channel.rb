require 'channel/channel_base.rb'
require 'channel/channel_server.rb'
class GateServer < ChannelServer


	class LogicChannel < ChannelBase

		def on_create(data=nil)
			return data
		end

	end


end