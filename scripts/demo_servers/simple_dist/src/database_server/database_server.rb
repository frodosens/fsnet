
require 'database_server/channel/owner_channel.rb'

class DatabaseServer < ChannelServer

	include PackTypeDefine

	def on_start_complete
		super

	end



end