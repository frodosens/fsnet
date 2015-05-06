
require 'database_server/channel/owner_channel.rb'
require 'database_server/game_database.rb'

class DatabaseServer < ChannelServer

	include PackTypeDefine

	attr_reader :database

	def on_start_complete
		super
		info("开始连接数据库 from #{@configure}")
		#@database = GameDatabase.new(self, @configure)
	end



end